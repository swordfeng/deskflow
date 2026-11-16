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
extern "C" void deskflowSetAuditSecondEnabled(bool);
extern "C" void deskflowSetAuditInputMethod(bool);
extern "C" int deskflowSendAuditNotification(bool);

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

  void selectedNotificationChangesTranslation_data()
  {
    QTest::addColumn<bool>("workerNotification");
    QTest::newRow("main-thread-notification") << false;
    QTest::newRow("worker-thread-notification") << true;
  }

  void selectedNotificationChangesTranslation()
  {
    QFETCH(bool, workerNotification);
    auto &sources = OSXInputSources::instance();
    deskflowSetAuditInputSource(0);
    sources.refresh();
    EventQueue events;
    deskflow::KeyMap keyMap;
    OSXKeyState keys(&events, keyMap, {"en", "fr"}, true);
    auto character = [&] {
      auto event = CGEventCreateKeyboardEvent(nullptr, kVK_ANSI_A, true);
      CGEventSetFlags(event, 0);
      OSXKeyState::KeyIDs ids;
      KeyModifierMask mask{};
      keys.mapKeyFromEvent(ids, &mask, event);
      CFRelease(event);
      return ids.size() == 1 ? ids.front() : kKeyNone;
    };
    QCOMPARE(character(), KeyID('a'));
    deskflowSetAuditInputSource(1);
    int delivered = 0;
    if (workerNotification) {
      std::thread worker([&] { delivered = deskflowSendAuditNotification(false); });
      worker.join();
    } else {
      delivered = deskflowSendAuditNotification(false);
    }
    QCOMPARE(delivered, 1);
    QTRY_COMPARE_WITH_TIMEOUT(sources.snapshot()->activeLayout, std::string("deskflow.test.b"), 1000);
    QCOMPARE(sources.snapshot()->language, std::string("fr"));
    QCOMPARE(character(), KeyID('q'));
    QCOMPARE(deskflowBackgroundInputSourceCalls(), 0);
  }

  void enabledNotificationRefreshesSourceList()
  {
    auto &sources = OSXInputSources::instance();
    deskflowSetAuditInputSource(0);
    sources.refresh();
    QCOMPARE(sources.snapshot()->layouts.size(), size_t(2));
    deskflowSetAuditSecondEnabled(false);
    const int delivered = deskflowSendAuditNotification(true);
    const auto disabled = sources.snapshot();
    deskflowSetAuditSecondEnabled(true);
    deskflowSendAuditNotification(true);
    QCOMPARE(delivered, 1);
    QCOMPARE(disabled->layouts.size(), size_t(1));
    QCOMPARE(disabled->languages, std::vector<std::string>({"en"}));
    QCOMPARE(sources.snapshot()->layouts.size(), size_t(2));
  }

  void inputMethodUsesAsciiFallback()
  {
    auto &sources = OSXInputSources::instance();
    deskflowSetAuditInputSource(1);
    deskflowSetAuditInputMethod(true);
    sources.refresh();
    auto snapshot = sources.snapshot();
    UInt32 state = 0;
    UniChar character = 0;
    UniCharCount count = 0;
    const auto status = UCKeyTranslate(
        reinterpret_cast<const UCKeyboardLayout *>(snapshot->translationData.constData()), kVK_ANSI_A, kUCKeyActionDown,
        0, 0, 0, &state, 1, &count, &character
    );
    deskflowSetAuditInputMethod(false);
    deskflowSetAuditInputSource(0);
    sources.refresh();
    QCOMPARE(snapshot->activeLayout, std::string("deskflow.test.b"));
    QCOMPARE(status, noErr);
    QCOMPARE(count, UniCharCount(1));
    QCOMPARE(character, UniChar('a'));
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
    deskflowSetAuditInputSource(0);
    OSXInputSources::instance().refresh();
    QCOMPARE(OSXInputSources::instance().snapshot()->activeLayout, std::string("deskflow.test.a"));
    const int before = deskflowInputSourceSelections();
    std::atomic<bool> done{false};
    bool result = false;
    std::string language;
    std::string activeLayout;
    auto worker = std::thread([&] {
      result = OSXInputSources::instance().select("deskflow.test.b");
      const auto snapshot = OSXInputSources::instance().snapshot();
      language = snapshot->language;
      activeLayout = snapshot->activeLayout;
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
    QCOMPARE(activeLayout, std::string("deskflow.test.b"));
    QCOMPARE(deskflowInputSourceSelections(), before + 1);
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
