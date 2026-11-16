/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <Carbon/Carbon.h>
#include <QByteArray>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Only copied values cross threads. No TIS references or borrowed properties escape.
struct OSXInputSourceLayout
{
  std::string id;
  QByteArray data;
};

struct OSXInputSourceSnapshot
{
  std::string language;
  std::vector<std::string> languages;
  std::vector<OSXInputSourceLayout> layouts;
  std::string activeLayout;
  QByteArray translationData;
};

class OSXInputSources
{
public:
  // Initialize on the main thread before starting the core/event-tap threads.
  static OSXInputSources &instance();
  std::shared_ptr<const OSXInputSourceSnapshot> snapshot() const;
  void refresh(); // Main thread only; also used by input-source notifications.
  bool select(const std::string &id, std::chrono::milliseconds timeout = std::chrono::milliseconds(250));

private:
  OSXInputSources();
  ~OSXInputSources() = default;
  bool selectOnMainThread(const std::string &id);
  static void notification(CFNotificationCenterRef, void *, CFStringRef, const void *, CFDictionaryRef);

  mutable std::mutex m_mutex;
  std::shared_ptr<const OSXInputSourceSnapshot> m_snapshot;
};
