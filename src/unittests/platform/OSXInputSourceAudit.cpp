/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include <Carbon/Carbon.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>
#include <vector>

namespace {
std::atomic<int> backgroundCalls{0};
std::atomic<int> selections{0};
std::atomic<int> active{0};
std::atomic<bool> secondEnabled{true};
std::atomic<bool> inputMethod{false};
CFDictionaryRef sources[2]{};

struct Observer
{
  CFNotificationCenterRef center;
  const void *observer;
  CFNotificationCallback callback;
  CFStringRef name;
};
std::vector<Observer> observers;

CFDataRef layoutData(UniChar character)
{
  // Minimal documented 'uchr' resource: one keyboard type and modifier table.
  struct Layout
  {
    UCKeyboardLayout header{};
    alignas(4) UCKeyModifiersToTableNum modifiers{};
    alignas(4) UCKeyToCharTableIndex index{};
    alignas(4) UCKeyOutput characters[128]{};
  } layout;
  layout.header.keyLayoutHeaderFormat = kUCKeyLayoutHeaderFormat;
  layout.header.keyLayoutDataVersion = 0x0100;
  layout.header.keyboardTypeCount = 1;
  layout.header.keyboardTypeList[0].keyModifiersToTableNumOffset = offsetof(Layout, modifiers);
  layout.header.keyboardTypeList[0].keyToCharTableIndexOffset = offsetof(Layout, index);
  layout.modifiers.keyModifiersToTableNumFormat = kUCKeyModifiersToTableNumFormat;
  layout.modifiers.modifiersCount = 1;
  layout.index.keyToCharTableIndexFormat = kUCKeyToCharTableIndexFormat;
  layout.index.keyToCharTableSize = 128;
  layout.index.keyToCharTableCount = 1;
  layout.index.keyToCharTableOffsets[0] = offsetof(Layout, characters);
  std::fill(std::begin(layout.characters), std::end(layout.characters), 0xffff);
  layout.characters[kVK_ANSI_A] = character;
  layout.characters[kVK_Space] = ' ';
  return CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(&layout), sizeof(layout));
}

bool testCaller(void *address)
{
  Dl_info info{};
  return sources[0] && dladdr(address, &info) && info.dli_fname && std::strstr(info.dli_fname, "OSXInputSourceTests");
}

void audit()
{
  if (!pthread_main_np())
    ++backgroundCalls;
}

TISInputSourceRef copyCurrent()
{
  if (!testCaller(__builtin_return_address(0)))
    return TISCopyCurrentKeyboardInputSource();
  audit();
  return reinterpret_cast<TISInputSourceRef>(const_cast<void *>(CFRetain(sources[active.load()])));
}

TISInputSourceRef copyLayout()
{
  if (!testCaller(__builtin_return_address(0)))
    return TISCopyCurrentKeyboardLayoutInputSource();
  audit();
  return reinterpret_cast<TISInputSourceRef>(const_cast<void *>(CFRetain(sources[active.load()])));
}

TISInputSourceRef copyAscii()
{
  if (!testCaller(__builtin_return_address(0)))
    return TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
  audit();
  return reinterpret_cast<TISInputSourceRef>(const_cast<void *>(CFRetain(sources[0])));
}

void *property(TISInputSourceRef source, CFStringRef key)
{
  if (!testCaller(__builtin_return_address(0)))
    return TISGetInputSourceProperty(source, key);
  audit();
  if (inputMethod && reinterpret_cast<CFDictionaryRef>(source) == sources[1]) {
    if (CFEqual(key, kTISPropertyUnicodeKeyLayoutData))
      return nullptr;
    if (CFEqual(key, kTISPropertyInputSourceIsSelectCapable))
      return const_cast<void *>(static_cast<const void *>(kCFBooleanFalse));
  }
  return const_cast<void *>(CFDictionaryGetValue(reinterpret_cast<CFDictionaryRef>(source), key));
}

CFArrayRef list(CFDictionaryRef filter, Boolean all)
{
  if (!testCaller(__builtin_return_address(0)))
    return TISCreateInputSourceList(filter, all);
  audit();
  const void *values[] = {sources[0], sources[1]};
  return CFArrayCreate(nullptr, values, secondEnabled || all ? 2 : 1, &kCFTypeArrayCallBacks);
}

void observe(
    CFNotificationCenterRef center, const void *observer, CFNotificationCallback callback, CFStringRef name,
    const void *object, CFNotificationSuspensionBehavior behavior
)
{
  if (!testCaller(__builtin_return_address(0))) {
    CFNotificationCenterAddObserver(center, observer, callback, name, object, behavior);
    return;
  }
  observers.push_back({center, observer, callback, name});
}

OSStatus select(TISInputSourceRef source)
{
  if (!testCaller(__builtin_return_address(0)))
    return TISSelectInputSource(source);
  audit();
  ++selections;
  active = reinterpret_cast<CFDictionaryRef>(source) == sources[1] ? 1 : 0;
  return noErr;
}

__attribute__((used)) static const struct
{
  const void *replacement;
  const void *original;
} interposes[] __attribute__((section("__DATA,__interpose"))) = {
    {reinterpret_cast<const void *>(&copyCurrent), reinterpret_cast<const void *>(&TISCopyCurrentKeyboardInputSource)},
    {reinterpret_cast<const void *>(&copyLayout),
     reinterpret_cast<const void *>(&TISCopyCurrentKeyboardLayoutInputSource)},
    {reinterpret_cast<const void *>(&copyAscii),
     reinterpret_cast<const void *>(&TISCopyCurrentASCIICapableKeyboardLayoutInputSource)},
    {reinterpret_cast<const void *>(&property), reinterpret_cast<const void *>(&TISGetInputSourceProperty)},
    {reinterpret_cast<const void *>(&list), reinterpret_cast<const void *>(&TISCreateInputSourceList)},
    {reinterpret_cast<const void *>(&select), reinterpret_cast<const void *>(&TISSelectInputSource)},
    {reinterpret_cast<const void *>(&observe), reinterpret_cast<const void *>(&CFNotificationCenterAddObserver)}
};
} // namespace

extern "C" void deskflowPrepareInputSourceAudit()
{
  const void *ids[] = {CFSTR("deskflow.test.a"), CFSTR("deskflow.test.b")};
  const void *languages[] = {CFSTR("en"), CFSTR("fr")};
  for (int i = 0; i < 2; ++i) {
    auto dict = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dict, kTISPropertyInputSourceID, ids[i]);
    auto array = CFArrayCreate(nullptr, &languages[i], 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(dict, kTISPropertyInputSourceLanguages, array);
    CFRelease(array);
    CFDictionarySetValue(dict, kTISPropertyInputSourceIsSelectCapable, kCFBooleanTrue);
    CFDictionarySetValue(dict, kTISPropertyInputSourceIsEnableCapable, kCFBooleanTrue);
    auto data = layoutData(i == 0 ? 'a' : 'q');
    CFDictionarySetValue(dict, kTISPropertyUnicodeKeyLayoutData, data);
    CFRelease(data);
    sources[i] = dict;
  }
  backgroundCalls = 0;
}

extern "C" int deskflowBackgroundInputSourceCalls()
{
  return backgroundCalls.load();
}
extern "C" void deskflowResetBackgroundInputSourceCalls()
{
  backgroundCalls = 0;
}
extern "C" int deskflowInputSourceSelections()
{
  return selections.load();
}
extern "C" void deskflowSetAuditInputSource(int index)
{
  active = index;
}

extern "C" void deskflowSetAuditSecondEnabled(bool enabled)
{
  secondEnabled = enabled;
}

extern "C" void deskflowSetAuditInputMethod(bool enabled)
{
  inputMethod = enabled;
}

extern "C" int deskflowSendAuditNotification(bool enabledSources)
{
  int delivered = 0;
  auto name =
      enabledSources ? kTISNotifyEnabledKeyboardInputSourcesChanged : kTISNotifySelectedKeyboardInputSourceChanged;
  for (const auto &observer : observers) {
    if (observer.center == CFNotificationCenterGetDistributedCenter() && CFEqual(name, observer.name)) {
      observer.callback(observer.center, const_cast<void *>(observer.observer), name, nullptr, nullptr);
      ++delivered;
    }
  }
  return delivered;
}
