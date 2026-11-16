/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"

#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename T> void put(std::string &data, size_t offset, T value)
{
  require(offset <= data.size() && sizeof(value) <= data.size() - offset, "invalid test fixture offset");
  memcpy(data.data() + offset, &value, sizeof(value));
}

std::string makeDib(DWORD headerSize, WORD bits, DWORD compression, LONG width = 1, LONG height = -2)
{
  const size_t masks = compression == BI_BITFIELDS && headerSize == sizeof(BITMAPINFOHEADER) ? 12 : 0;
  const size_t colors = bits <= 8 ? size_t{1} << bits : 0;
  const size_t stride = ((static_cast<size_t>(width) * bits + 31) / 32) * 4;
  const size_t rows = height < 0 ? -static_cast<int64_t>(height) : height;
  std::string data(headerSize + masks + colors * sizeof(RGBQUAD) + stride * rows, '\0');
  BITMAPINFOHEADER header{};
  header.biSize = headerSize;
  header.biWidth = width;
  header.biHeight = height;
  header.biPlanes = 1;
  header.biBitCount = bits;
  header.biCompression = compression;
  header.biSizeImage = static_cast<DWORD>(stride * rows);
  memcpy(data.data(), &header, sizeof(header));
  if (compression == BI_BITFIELDS) {
    put<DWORD>(data, 40, bits == 16 ? 0xf800 : 0x00ff0000);
    put<DWORD>(data, 44, bits == 16 ? 0x07e0 : 0x0000ff00);
    put<DWORD>(data, 48, bits == 16 ? 0x001f : 0x000000ff);
  }
  if (headerSize >= sizeof(BITMAPV4HEADER)) {
    put<DWORD>(data, 56, LCS_sRGB);
  }
  return data;
}

struct GlobalMemory
{
  HGLOBAL handle;
  explicit GlobalMemory(const std::string &data) : handle(GlobalAlloc(GMEM_MOVEABLE, data.size()))
  {
    require(handle != nullptr, "GlobalAlloc failed");
    if (data.empty()) {
      return; // A zero-size movable allocation is discarded and cannot be locked.
    }
    auto *dst = GlobalLock(handle);
    require(dst != nullptr, "GlobalLock failed");
    memcpy(dst, data.data(), data.size());
    GlobalUnlock(handle);
  }
  ~GlobalMemory()
  {
    GlobalFree(handle);
  }
  GlobalMemory(const GlobalMemory &) = delete;
  GlobalMemory &operator=(const GlobalMemory &) = delete;
};

std::string convert(const std::string &data)
{
  GlobalMemory memory(data);
  const auto result = MSWindowsClipboardBitmapConverter().toIClipboard(memory.handle);
  require((GlobalFlags(memory.handle) & GMEM_LOCKCOUNT) == 0, "input memory left locked");
  return result;
}

void reject(const std::string &data)
{
  MSWindowsClipboardBitmapConverter converter;
  const auto published = converter.fromIClipboard(data);
  if (published != nullptr) {
    GlobalFree(published);
    throw std::runtime_error("invalid DIB accepted for publication");
  }
  require(convert(data).empty(), "invalid DIB accepted for native conversion");
}

void bitfieldsUseCorrectPixels()
{
  for (const DWORD size : {sizeof(BITMAPINFOHEADER), sizeof(BITMAPV4HEADER), sizeof(BITMAPV5HEADER)}) {
    for (const LONG height : {-2, 2}) {
      auto data = makeDib(size, 32, BI_BITFIELDS, 1, height);
      const auto offset = size == sizeof(BITMAPINFOHEADER) ? size + 12 : size;
      const unsigned char pixels[] = {0x12, 0x34, 0x56, 0xff, 0x78, 0x9a, 0xbc, 0xff};
      memcpy(data.data() + offset, pixels, sizeof(pixels));
      const auto result = convert(data);
      require(result.size() == sizeof(BITMAPINFOHEADER) + sizeof(pixels), "wrong converted image size");
      BITMAPINFOHEADER header;
      memcpy(&header, result.data(), sizeof(header));
      require(header.biHeight == height && header.biCompression == BI_RGB, "wrong output orientation/format");
      for (size_t row = 0; row != 2; ++row) {
        require(
            memcmp(result.data() + sizeof(BITMAPINFOHEADER) + row * 4, pixels + row * 4, 3) == 0,
            "wrong RGB pixels: embedded masks must not be skipped twice"
        );
      }
    }
  }
}

void healthyPublicationAndLegacyNormalisation()
{
  MSWindowsClipboardBitmapConverter converter;
  const auto data = makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS);
  const auto handle = converter.fromIClipboard(data);
  require(handle != nullptr, "healthy V5 bitmap rejected");
  const auto *src = static_cast<const char *>(GlobalLock(handle));
  const bool equal = src != nullptr && memcmp(src, data.data(), data.size()) == 0;
  if (src != nullptr) {
    GlobalUnlock(handle);
  }
  GlobalFree(handle);
  require(equal, "healthy V5 bitmap changed");

  auto legacy = makeDib(sizeof(BITMAPINFOHEADER), 32, BI_RGB);
  put<DWORD>(legacy, 0, sizeof(BITMAPV5HEADER));
  put<DWORD>(legacy, 16, BI_BITFIELDS);
  const auto normalised = converter.fromIClipboard(legacy);
  require(normalised != nullptr, "legacy malformed Mac DIB no longer repaired");
  const auto result = converter.toIClipboard(normalised);
  GlobalFree(normalised);
  require(result == makeDib(sizeof(BITMAPINFOHEADER), 32, BI_RGB), "incorrect legacy normalisation");
}

void rgbPreservesPaddingAndOrientation()
{
  for (const WORD bits : {24, 32}) {
    for (const LONG height : {-2, 2}) {
      const auto data = makeDib(sizeof(BITMAPINFOHEADER), bits, BI_RGB, 3, height);
      const auto result = convert(data);
      // GlobalAlloc may round up its size. Only the actual DIB bytes matter.
      require(result.size() >= data.size() && result.compare(0, data.size(), data) == 0, "RGB data changed");
    }
  }
}

void truncatedHeadersMasksPalettesAndPixels()
{
  for (const auto &data :
       {makeDib(sizeof(BITMAPINFOHEADER), 32, BI_RGB), makeDib(sizeof(BITMAPINFOHEADER), 16, BI_BITFIELDS),
        makeDib(sizeof(BITMAPV4HEADER), 32, BI_BITFIELDS), makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS),
        makeDib(sizeof(BITMAPINFOHEADER), 8, BI_RGB)}) {
    // Reject on the wire using its exact length. HGLOBAL may round its size
    // up, so test native conversion with buffers missing at least 32 bytes.
    for (size_t length = 0; length < data.size(); ++length) {
      MSWindowsClipboardBitmapConverter converter;
      auto truncated = data.substr(0, length);
      // The legacy Mac repair deliberately accepts a 40-byte payload whose
      // header claims V5. It is indistinguishable from this exact truncation.
      DWORD declaredSize;
      memcpy(&declaredSize, data.data(), sizeof(declaredSize));
      if (declaredSize == sizeof(BITMAPV5HEADER) && length == sizeof(BITMAPINFOHEADER) + 8) {
        continue;
      }
      const auto handle = converter.fromIClipboard(truncated);
      if (handle != nullptr) {
        GlobalFree(handle);
        throw std::runtime_error("truncated wire DIB accepted");
      }
      if (length + 32 <= data.size()) {
        require(convert(truncated).empty(), "truncated native DIB accepted");
      }
    }
  }
}

void invalidDimensionsAndMetadata()
{
  const auto valid = makeDib(sizeof(BITMAPINFOHEADER), 32, BI_RGB);
  for (const auto &[offset, value] : std::vector<std::pair<size_t, DWORD>>{
           {0, 12},
           {0, 0xffffffff},
           {4, 0},
           {4, 0xffffffff},
           {4, 0x7fffffff},
           {8, 0},
           {8, 0x80000000},
           {8, 0x7fffffff},
           {12, 0x00200002},
           {14, 0x00000003},
           {16, BI_RLE8},
           {16, 0xffffffff},
           {20, 1},
           {20, 0xffffffff},
           {32, 0xffffffff}
       }) {
    auto bad = valid;
    put<DWORD>(bad, offset, value);
    reject(bad);
  }
  auto indexed = makeDib(sizeof(BITMAPINFOHEADER), 8, BI_RGB);
  put<DWORD>(indexed, 32, 257);
  reject(indexed);
  for (const DWORD mask : {0, 0x0000ff00, 0x00f000f0}) {
    auto bad = makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS);
    put<DWORD>(bad, 40, mask);
    reject(bad);
  }
}

void invalidProfiles()
{
  auto data = makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS);
  put<DWORD>(data, 56, PROFILE_EMBEDDED);
  put<DWORD>(data, 112, 0xfffffff0);
  put<DWORD>(data, 116, 64);
  reject(data);
  put<DWORD>(data, 112, sizeof(BITMAPV5HEADER));
  put<DWORD>(data, 116, 4);
  reject(data); // profile overlaps pixels
  put<DWORD>(data, 112, static_cast<DWORD>(data.size()));
  put<DWORD>(data, 116, 0);
  reject(data);
  put<DWORD>(data, 56, PROFILE_LINKED);
  put<DWORD>(data, 116, 32);
  data.append(32, 'x');
  reject(data); // linked filename is not terminated
}

void indexedAnd16BitConversion()
{
  auto indexed = makeDib(sizeof(BITMAPINFOHEADER), 8, BI_RGB);
  put<DWORD>(indexed, sizeof(BITMAPINFOHEADER), 0x00563412);
  auto result = convert(indexed);
  require(result.size() == 48 && memcmp(result.data() + 40, "\x12\x34\x56", 3) == 0, "palette conversion failed");
  auto rgb16 = makeDib(sizeof(BITMAPINFOHEADER), 16, BI_BITFIELDS);
  put<WORD>(rgb16, 52, 0xf800);
  result = convert(rgb16);
  require(result.size() == 48 && static_cast<unsigned char>(result[42]) == 0xff, "16-bit conversion failed");
}

void largeV5Image()
{
  auto data = makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS, 4096, -2160);
  data[sizeof(BITMAPV5HEADER)] = 0x12;
  data[sizeof(BITMAPV5HEADER) + 1] = 0x34;
  data[sizeof(BITMAPV5HEADER) + 2] = 0x56;
  const auto result = convert(data);
  require(result.size() == 40 + 4096 * 2160 * 4, "large image conversion failed");
  require(memcmp(result.data() + 40, "\x12\x34\x56", 3) == 0, "large image pixel offset is incorrect");
}

void repeatedConversionReleasesGdiObjects()
{
  const auto data = makeDib(sizeof(BITMAPV5HEADER), 32, BI_BITFIELDS);
  convert(data); // warm up GDI
  const auto before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  for (int i = 0; i < 100; ++i) {
    require(!convert(data).empty(), "repeated conversion failed");
  }
  const auto after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  require(after == before, "GDI resources leaked");
}
} // namespace

int main()
{
  Log log;
  const std::vector<std::pair<const char *, std::function<void()>>> tests{
      {"bitfieldsUseCorrectPixels", bitfieldsUseCorrectPixels},
      {"healthyPublicationAndLegacyNormalisation", healthyPublicationAndLegacyNormalisation},
      {"rgbPreservesPaddingAndOrientation", rgbPreservesPaddingAndOrientation},
      {"truncatedHeadersMasksPalettesAndPixels", truncatedHeadersMasksPalettesAndPixels},
      {"invalidDimensionsAndMetadata", invalidDimensionsAndMetadata},
      {"invalidProfiles", invalidProfiles},
      {"indexedAnd16BitConversion", indexedAnd16BitConversion},
      {"largeV5Image", largeV5Image},
      {"repeatedConversionReleasesGdiObjects", repeatedConversionReleasesGdiObjects}
  };
  int failures = 0;
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
