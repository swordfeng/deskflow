/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include <Carbon/Carbon.h>
#include <atomic>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

namespace {
std::atomic<int> backgroundCalls{0};
std::atomic<int> selections{0};
std::atomic<int> active{0};
CFDictionaryRef sources[2]{};

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
  return reinterpret_cast<TISInputSourceRef>(const_cast<void *>(CFRetain(sources[active.load()])));
}

void *property(TISInputSourceRef source, CFStringRef key)
{
  if (!testCaller(__builtin_return_address(0)))
    return TISGetInputSourceProperty(source, key);
  audit();
  return const_cast<void *>(CFDictionaryGetValue(reinterpret_cast<CFDictionaryRef>(source), key));
}

CFArrayRef list(CFDictionaryRef filter, Boolean all)
{
  if (!testCaller(__builtin_return_address(0)))
    return TISCreateInputSourceList(filter, all);
  audit();
  const void *values[] = {sources[0], sources[1]};
  return CFArrayCreate(nullptr, values, 2, &kCFTypeArrayCallBacks);
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
    {reinterpret_cast<const void *>(&select), reinterpret_cast<const void *>(&TISSelectInputSource)}
};
} // namespace

extern "C" void deskflowPrepareInputSourceAudit()
{
  // Calls from this interposing library itself resolve to the original APIs.
  auto realSource = TISCopyCurrentKeyboardLayoutInputSource();
  auto data = realSource ? TISGetInputSourceProperty(realSource, kTISPropertyUnicodeKeyLayoutData) : nullptr;
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
    if (data)
      CFDictionarySetValue(dict, kTISPropertyUnicodeKeyLayoutData, data);
    sources[i] = dict;
  }
  if (realSource)
    CFRelease(realSource);
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
