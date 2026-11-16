/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "deskflow/unix/AppUtilUnix.h"
#include "platform/IOSXKeyResource.h"
#include "platform/OSXInputSources.h"
#include "platform/OSXKeyState.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTest>
#include <QTimer>
#include <atomic>
#include <thread>

extern "C" void deskflowPrepareInputSourceAudit();
extern "C" int deskflowBackgroundInputSourceCalls();
extern "C" void deskflowResetBackgroundInputSourceCalls();
extern "C" int deskflowInputSourceSelections();
extern "C" void deskflowSetAuditInputSource(int);

class OSXInputSourceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    deskflowPrepareInputSourceAudit();
    m_arch.init();
    m_log.setFilter(LogLevel::Level::Error);
    OSXInputSources::instance();
  }

  void snapshotsSurviveRefresh()
  {
    auto &sources = OSXInputSources::instance();
    deskflowSetAuditInputSource(0);
    sources.refresh();
    auto old = sources.snapshot();
    QCOMPARE(old->language, std::string("en"));
    QCOMPARE(old->activeLayout, std::string("deskflow.test.a"));
    QCOMPARE(old->layouts.size(), size_t(2));
    deskflowSetAuditInputSource(1);
    sources.refresh();
    auto current = sources.snapshot();
    QCOMPARE(current->language, std::string("fr"));
    QCOMPARE(current->activeLayout, std::string("deskflow.test.b"));
    QCOMPARE(old->language, std::string("en"));
    QCOMPARE(old->activeLayout, std::string("deskflow.test.a"));
  }

  void auditDetectsUnsafeBackgroundAccess()
  {
    deskflowResetBackgroundInputSourceCalls();
    std::thread worker([] {
      auto source = TISCopyCurrentKeyboardInputSource();
      CFRelease(source);
    });
    worker.join();
    QCOMPARE(deskflowBackgroundInputSourceCalls(), 1);
    deskflowResetBackgroundInputSourceCalls();
  }

  void workerReadsDuringRefresh_data()
  {
    QTest::addColumn<bool>("languageSync");
    QTest::newRow("sync-disabled") << false;
    QTest::newRow("sync-enabled") << true;
  }

  void workerReadsDuringRefresh()
  {
    QFETCH(bool, languageSync);
    EventQueue events;
    AppUtilUnix util(&events);
    deskflow::KeyMap keyMap;
    OSXKeyState keys(&events, keyMap, {"en", "fr"}, languageSync);
    std::atomic<int> failures{0};
    std::atomic<bool> start{false};
    auto worker = std::thread([&] {
      while (!start.load())
        std::this_thread::yield();
      keys.updateKeyMap();
      for (int i = 0; i < 2000; ++i) {
        auto snapshot = OSXInputSources::instance().snapshot();
        if (!((snapshot->language == "en" && snapshot->activeLayout == "deskflow.test.a") ||
              (snapshot->language == "fr" && snapshot->activeLayout == "deskflow.test.b")))
          ++failures;
        auto lang = util.getCurrentLanguageCode();
        if (lang != "en" && lang != "fr")
          ++failures;
        if (util.getKeyboardLayoutList().size() != 2 || keys.pollActiveGroup() < 0)
          ++failures;
        // Exercise native key translation without posting any key events to other apps.
        auto event = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_A, true);
        OSXKeyState::KeyIDs ids;
        KeyModifierMask mask{};
        if (keys.mapKeyFromEvent(ids, &mask, event) == 0 || ids.empty())
          ++failures;
        CFRelease(event);
        IOSXKeyResource::getKeyID(214);
      }
    });
    start = true;
    for (int i = 0; i < 100; ++i) {
      deskflowSetAuditInputSource(i % 2);
      OSXInputSources::instance().refresh();
    }
    worker.join();
    QCOMPARE(failures.load(), 0);
    // The final refresh selects the second layout: a fallback to group zero must fail.
    QCOMPARE(keys.pollActiveGroup(), 1);
    QCOMPARE(deskflowBackgroundInputSourceCalls(), 0);
  }

  void workerSelectionPublishesBeforeReturning()
  {
    std::atomic<bool> done{false};
    bool result = false;
    std::string language;
    auto worker = std::thread([&] {
      result = OSXInputSources::instance().select("deskflow.test.b");
      language = OSXInputSources::instance().snapshot()->language;
      done = true;
    });
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done && elapsed.elapsed() < 2000) {
      QCoreApplication::processEvents();
      QTest::qSleep(1);
    }
    worker.join();
    QVERIFY(result);
    QCOMPARE(language, std::string("fr"));
    QCOMPARE(deskflowBackgroundInputSourceCalls(), 0);
  }

  void shutdownTimeoutCancelsQueuedSelection()
  {
    const int before = deskflowInputSourceSelections();
    bool result = true;
    QElapsedTimer timer;
    timer.start();
    std::thread worker([&] {
      result = OSXInputSources::instance().select("deskflow.test.a", std::chrono::milliseconds(25));
    });
    // Model shutdown: the main thread waits for the worker without pumping events.
    worker.join();
    QVERIFY(!result);
    QVERIFY(timer.elapsed() < 1000);
    QCoreApplication::processEvents();
    QCOMPARE(deskflowInputSourceSelections(), before);
  }

private:
  Arch m_arch;
  Log m_log;
};

QTEST_MAIN(OSXInputSourceTests)
#include "OSXInputSourceTests.moc"
