/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboardBitmapConverter.h"

#include "base/Log.h"

#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
struct DibLayout
{
  BITMAPINFOHEADER header{};
  size_t pixelOffset = 0;
  size_t outputBytes = 0;
  LONG height = 0;
};

bool validMask(DWORD mask, WORD bits)
{
  if (mask == 0 || (bits < 32 && (mask >> bits) != 0)) {
    return false;
  }
  const auto shifted = mask >> std::countr_zero(mask);
  return (shifted & (shifted + 1)) == 0;
}

// Validate every range GDI can read before passing it a pointer. Use a copy of
// the fixed header: incoming strings need not be aligned for Windows structs.
bool parseDib(const void *data, size_t size, DibLayout &layout)
{
  if (size < sizeof(BITMAPINFOHEADER) || size > (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }
  memcpy(&layout.header, data, sizeof(layout.header));
  const auto &header = layout.header;
  if ((header.biSize != sizeof(BITMAPINFOHEADER) && header.biSize != sizeof(BITMAPV4HEADER) &&
       header.biSize != sizeof(BITMAPV5HEADER)) ||
      header.biSize > size || header.biPlanes != 1 || header.biWidth <= 0 || header.biHeight == 0 ||
      header.biHeight == (std::numeric_limits<LONG>::min)()) {
    return false;
  }
  const auto bits = header.biBitCount;
  if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32) {
    return false;
  }
  // RLE/JPEG/PNG DIBs need their own bounded decoder. Do not hand arbitrary
  // compressed streams to GDI on this raw-bitmap clipboard path.
  if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS) {
    return false;
  }
  size_t offset = header.biSize;
  const auto *bytes = static_cast<const char *>(data);
  if (header.biCompression == BI_BITFIELDS) {
    if (bits != 16 && bits != 32) {
      return false;
    }
    // Only INFOHEADER stores masks after the header. V4/V5 embed them.
    if (header.biSize == sizeof(BITMAPINFOHEADER)) {
      offset += 3 * sizeof(DWORD);
    }
    if (offset > size) {
      return false;
    }
    DWORD masks[3];
    memcpy(masks, bytes + sizeof(BITMAPINFOHEADER), sizeof(masks));
    if (!validMask(masks[0], bits) || !validMask(masks[1], bits) || !validMask(masks[2], bits) ||
        (masks[0] & masks[1]) != 0 || (masks[0] & masks[2]) != 0 || (masks[1] & masks[2]) != 0) {
      return false;
    }
  }
  uint64_t colors = header.biClrUsed;
  if (bits <= 8) {
    const auto maximum = uint64_t{1} << bits;
    if (colors > maximum) {
      return false;
    }
    if (colors == 0) {
      colors = maximum;
    }
  }
  const auto paletteBytes = colors * sizeof(RGBQUAD);
  if (paletteBytes > size - offset) {
    return false;
  }
  offset += static_cast<size_t>(paletteBytes);
  layout.height = header.biHeight < 0 ? -header.biHeight : header.biHeight;
  const auto width = static_cast<uint64_t>(header.biWidth);
  const auto height = static_cast<uint64_t>(layout.height);
  const auto stride = ((width * bits + 31) / 32) * 4;
  const auto pixelBytes = stride * height;
  const auto outputBytes = width * height * 4;
  if (pixelBytes > size - offset || outputBytes > (std::numeric_limits<uint32_t>::max)() - sizeof(BITMAPINFOHEADER) ||
      (header.biSizeImage != 0 && (header.biSizeImage < pixelBytes || header.biSizeImage > size - offset))) {
    return false;
  }
  if (header.biSize == sizeof(BITMAPV5HEADER)) {
    BITMAPV5HEADER v5;
    memcpy(&v5, data, sizeof(v5));
    if (v5.bV5ProfileData != 0 || v5.bV5ProfileSize != 0 || v5.bV5CSType == PROFILE_LINKED ||
        v5.bV5CSType == PROFILE_EMBEDDED) {
      // A clipboard DIB is packed: profiles follow its pixels, and offsets
      // are relative to the start of the V5 header.
      const auto profileOffset = static_cast<size_t>(v5.bV5ProfileData);
      if (profileOffset < offset + pixelBytes || profileOffset > size || v5.bV5ProfileSize == 0 ||
          v5.bV5ProfileSize > size - profileOffset ||
          (v5.bV5CSType == PROFILE_LINKED && memchr(bytes + profileOffset, '\0', v5.bV5ProfileSize) == nullptr)) {
        return false;
      }
    }
  }
  layout.pixelOffset = offset;
  layout.outputBytes = static_cast<size_t>(outputBytes);
  return true;
}

struct BitmapResources
{
  HGLOBAL source;
  HDC screen = nullptr;
  HDC memory = nullptr;
  HBITMAP bitmap = nullptr;
  HGDIOBJ previous = nullptr;

  ~BitmapResources()
  {
    if (previous != nullptr && previous != HGDI_ERROR) {
      SelectObject(memory, previous);
    }
    if (memory != nullptr) {
      DeleteDC(memory);
    }
    if (bitmap != nullptr) {
      DeleteObject(bitmap);
    }
    if (screen != nullptr) {
      ReleaseDC(nullptr, screen);
    }
    GlobalUnlock(source);
  }
};

bool normaliseMalformedMacDib(const std::string &data, std::string &normalisedData)
{
  if (data.size() < sizeof(BITMAPINFOHEADER)) {
    return false;
  }

  BITMAPINFOHEADER header;
  memcpy(&header, data.data(), sizeof(header));
  if (header.biWidth <= 0) {
    return false;
  }

  const auto width = static_cast<size_t>(header.biWidth);
  const auto height = static_cast<size_t>(std::abs(static_cast<int64_t>(header.biHeight)));
  if (height == 0 || width > ((std::numeric_limits<size_t>::max)() - sizeof(BITMAPINFOHEADER)) / 4 / height) {
    return false;
  }
  const auto expectedSize = sizeof(BITMAPINFOHEADER) + width * height * 4;

  // macOS can describe an INFOHEADER-sized 32-bit pixel payload as a V5 DIB.
  // Windows then interprets the first pixels as V5 colour masks. The pixel
  // bytes are ordinary BGRA, so publish a canonical BI_RGB DIB instead.
  if (header.biSize != sizeof(BITMAPV5HEADER) || header.biPlanes != 1 || header.biBitCount != 32 ||
      header.biCompression != BI_BITFIELDS || header.biClrUsed != 0 || expectedSize != data.size()) {
    return false;
  }

  normalisedData = data;
  header.biSize = sizeof(BITMAPINFOHEADER);
  header.biCompression = BI_RGB;
  memcpy(normalisedData.data(), &header, sizeof(header));
  LOG_INFO("normalised malformed macOS clipboard image to BI_RGB");
  return true;
}
} // namespace

//
// MSWindowsClipboardBitmapConverter
//

IClipboard::Format MSWindowsClipboardBitmapConverter::getFormat() const
{
  return IClipboard::Format::Bitmap;
}

UINT MSWindowsClipboardBitmapConverter::getWin32Format() const
{
  return CF_DIB;
}

HANDLE MSWindowsClipboardBitmapConverter::fromIClipboard(const std::string &data) const
try {
  if (data.size() > (std::numeric_limits<uint32_t>::max)()) {
    LOG_WARN("clipboard bitmap is too large");
    return nullptr;
  }
  std::string normalisedData;
  const auto *clipboardData = &data;
  if (normaliseMalformedMacDib(data, normalisedData)) {
    clipboardData = &normalisedData;
  }
  DibLayout layout;
  if (!parseDib(clipboardData->data(), clipboardData->size(), layout)) {
    LOG_WARN("rejecting invalid or unsupported clipboard DIB (%zu bytes)", clipboardData->size());
    return nullptr;
  }

  HGLOBAL result = GlobalAlloc(GMEM_MOVEABLE, clipboardData->size());
  if (result != nullptr) {
    auto *dst = GlobalLock(result);
    if (dst != nullptr) {
      memcpy(dst, clipboardData->data(), clipboardData->size());
      GlobalUnlock(result);
    } else {
      GlobalFree(result);
      result = nullptr;
    }
  }
  return result;
} catch (const std::bad_alloc &) {
  LOG_WARN("not enough memory for clipboard bitmap");
  return nullptr;
} catch (const std::length_error &) {
  LOG_WARN("clipboard bitmap is too large");
  return nullptr;
}

std::string MSWindowsClipboardBitmapConverter::toIClipboard(HANDLE data) const
try {
  const auto size = GlobalSize(data);
  // Preserve SIZE_T: truncating GlobalSize to uint32_t can hide an overflow.
  if (size < sizeof(BITMAPINFOHEADER) || size > (std::numeric_limits<uint32_t>::max)()) {
    LOG_WARN("rejecting invalid clipboard bitmap size (%zu bytes)", size);
    return {};
  }
  const auto *src = static_cast<const char *>(GlobalLock(data));
  if (src == nullptr) {
    return {};
  }
  BitmapResources resources{data};
  DibLayout layout;
  if (!parseDib(src, size, layout)) {
    LOG_WARN("rejecting invalid or unsupported clipboard DIB (%zu bytes)", size);
    return {};
  }
  const auto &header = layout.header;
  LOG_INFO("bitmap: %ldx%ld %u", header.biWidth, header.biHeight, static_cast<unsigned>(header.biBitCount));
  if ((header.biBitCount == 24 || header.biBitCount == 32) && header.biCompression == BI_RGB) {
    return std::string(src, size);
  }

  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = header.biWidth;
  info.bmiHeader.biHeight = header.biHeight;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  info.bmiHeader.biSizeImage = static_cast<DWORD>(layout.outputBytes);
  info.bmiHeader.biXPelsPerMeter = 1000;
  info.bmiHeader.biYPelsPerMeter = 1000;

  resources.screen = GetDC(nullptr);
  if (resources.screen == nullptr) {
    LOG_WARN("failed to acquire device context for clipboard image");
    return {};
  }
  void *raw = nullptr;
  resources.bitmap = CreateDIBSection(resources.screen, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
  if (resources.bitmap == nullptr || raw == nullptr) {
    LOG_WARN("failed to allocate destination bitmap for clipboard image");
    return {};
  }
  resources.memory = CreateCompatibleDC(resources.screen);
  if (resources.memory == nullptr) {
    LOG_WARN("failed to create device context for clipboard image");
    return {};
  }
  resources.previous = SelectObject(resources.memory, resources.bitmap);
  if (resources.previous == nullptr || resources.previous == HGDI_ERROR) {
    LOG_WARN("failed to select destination bitmap for clipboard image");
    return {};
  }
  const auto lines = SetDIBitsToDevice(
      resources.memory, 0, 0, header.biWidth, layout.height, 0, 0, 0, layout.height, src + layout.pixelOffset,
      reinterpret_cast<const BITMAPINFO *>(src), DIB_RGB_COLORS
  );
  if (lines != layout.height || !GdiFlush()) {
    LOG_WARN("failed to convert clipboard bitmap");
    return {};
  }
  std::string image(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
  image.append(static_cast<const char *>(raw), layout.outputBytes);
  return image;
} catch (const std::bad_alloc &) {
  LOG_WARN("not enough memory for clipboard bitmap");
  return {};
} catch (const std::length_error &) {
  LOG_WARN("clipboard bitmap is too large");
  return {};
}
