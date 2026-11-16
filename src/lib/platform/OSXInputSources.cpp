/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXInputSources.h"
#include "OSXAutoTypes.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <algorithm>
#include <condition_variable>
#include <pthread.h>

namespace {
void requireMainThread()
{
  if (!pthread_main_np())
    qFatal("TextInputSources must only be accessed on the main thread");
}

std::string stringValue(CFStringRef value)
{
  if (!value)
    return {};
  const auto size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
  std::string result(size, '\0');
  if (!CFStringGetCString(value, result.data(), size, kCFStringEncodingUTF8))
    return {};
  result.resize(std::char_traits<char>::length(result.c_str()));
  return result;
}

std::string sourceString(TISInputSourceRef source, CFStringRef property)
{
  return source ? stringValue(static_cast<CFStringRef>(TISGetInputSourceProperty(source, property))) : "";
}

QByteArray layoutData(TISInputSourceRef source)
{
  auto data =
      source ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;
  return data ? QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data))
              : QByteArray();
}

std::string language(TISInputSourceRef source)
{
  auto values =
      source ? static_cast<CFArrayRef>(TISGetInputSourceProperty(source, kTISPropertyInputSourceLanguages)) : nullptr;
  for (CFIndex i = 0; values && i < CFArrayGetCount(values); ++i) {
    auto result = stringValue(static_cast<CFStringRef>(CFArrayGetValueAtIndex(values, i)));
    if (!result.empty())
      return result;
  }
  return {};
}
} // namespace

OSXInputSources &OSXInputSources::instance()
{
  static OSXInputSources sources;
  return sources;
}

OSXInputSources::OSXInputSources()
{
  requireMainThread();
  auto center = CFNotificationCenterGetDistributedCenter();
  for (auto name : {kTISNotifySelectedKeyboardInputSourceChanged, kTISNotifyEnabledKeyboardInputSourcesChanged})
    CFNotificationCenterAddObserver(
        center, this, notification, name, nullptr, CFNotificationSuspensionBehaviorDeliverImmediately
    );
  refresh();
}

OSXInputSources::~OSXInputSources()
{
  CFNotificationCenterRemoveEveryObserver(CFNotificationCenterGetDistributedCenter(), this);
}

void OSXInputSources::notification(CFNotificationCenterRef, void *observer, CFStringRef, const void *, CFDictionaryRef)
{
  auto sources = static_cast<OSXInputSources *>(observer);
  if (pthread_main_np())
    sources->refresh();
  else if (auto app = QCoreApplication::instance())
    QMetaObject::invokeMethod(app, [sources] { sources->refresh(); }, Qt::QueuedConnection);
}

std::shared_ptr<const OSXInputSourceSnapshot> OSXInputSources::snapshot() const
{
  std::lock_guard lock(m_mutex);
  return m_snapshot;
}

void OSXInputSources::refresh()
{
  requireMainThread();
  auto next = std::make_shared<OSXInputSourceSnapshot>();
  AutoTISInputSourceRef current(TISCopyCurrentKeyboardInputSource(), CFRelease);
  AutoTISInputSourceRef layout(TISCopyCurrentKeyboardLayoutInputSource(), CFRelease);
  next->language = language(current.get());
  next->activeLayout = sourceString(layout.get(), kTISPropertyInputSourceID);

  // Preserve the existing ASCII-layout fallback for input methods without a key layout.
  auto selectable =
      layout
          ? static_cast<CFBooleanRef>(TISGetInputSourceProperty(layout.get(), kTISPropertyInputSourceIsSelectCapable))
          : nullptr;
  if (current && layout && layoutData(current.get()).isEmpty() && !(selectable && CFBooleanGetValue(selectable))) {
    AutoTISInputSourceRef ascii(TISCopyCurrentASCIICapableKeyboardLayoutInputSource(), CFRelease);
    next->translationData = layoutData(ascii ? ascii.get() : layout.get());
  } else {
    next->translationData = layoutData(layout.get());
  }

  const void *keys[] = {kTISPropertyInputSourceCategory};
  const void *values[] = {kTISCategoryKeyboardInputSource};
  AutoCFDictionary filter(CFDictionaryCreate(nullptr, keys, values, 1, nullptr, nullptr), CFRelease);
  AutoCFArray sources(TISCreateInputSourceList(filter.get(), false), CFRelease);
  for (CFIndex i = 0; sources && i < CFArrayGetCount(sources.get()); ++i) {
    auto source = static_cast<TISInputSourceRef>(const_cast<void *>(CFArrayGetValueAtIndex(sources.get(), i)));
    next->layouts.push_back({sourceString(source, kTISPropertyInputSourceID), layoutData(source)});
    auto code = language(source);
    if (code.size() == 2 && std::find(next->languages.begin(), next->languages.end(), code) == next->languages.end())
      next->languages.push_back(std::move(code));
  }
  std::lock_guard lock(m_mutex);
  m_snapshot = std::move(next);
}

bool OSXInputSources::selectOnMainThread(const std::string &id)
{
  requireMainThread();
  AutoCFArray sources(TISCreateInputSourceList(nullptr, false), CFRelease);
  for (CFIndex i = 0; sources && i < CFArrayGetCount(sources.get()); ++i) {
    auto source = static_cast<TISInputSourceRef>(const_cast<void *>(CFArrayGetValueAtIndex(sources.get(), i)));
    if (sourceString(source, kTISPropertyInputSourceID) != id)
      continue;
    if (TISSelectInputSource(source) != noErr)
      return false;
    refresh(); // Publish before acknowledging; do not wait for a distributed notification.
    return true;
  }
  return false;
}

bool OSXInputSources::select(const std::string &id, std::chrono::milliseconds timeout)
{
  if (pthread_main_np())
    return selectOnMainThread(id);
  auto app = QCoreApplication::instance();
  if (!app || QCoreApplication::closingDown())
    return false;
  struct Request
  {
    std::mutex mutex;
    std::condition_variable ready;
    bool cancelled = false;
    bool done = false;
    bool result = false;
  };
  auto request = std::make_shared<Request>();
  if (!QMetaObject::invokeMethod(
          app,
          [this, request, id] {
            std::lock_guard lock(request->mutex);
            if (request->cancelled)
              return;
            request->result = selectOnMainThread(id);
            request->done = true;
            request->ready.notify_one();
          },
          Qt::QueuedConnection
      ))
    return false;
  std::unique_lock lock(request->mutex);
  if (!request->ready.wait_for(lock, timeout, [&] { return request->done; })) {
    request->cancelled = true;
    return false;
  }
  return request->result;
}
