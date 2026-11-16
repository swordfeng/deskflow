/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/Arch.h"
#include "base/EventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/PacketStreamFilter.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "deskflow/StreamChunker.h"
#include "deskflow/ipc/CoreIpcServer.h"
#include "net/FingerprintDatabase.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "net/NetworkAddress.h"
#include "net/SecureUtils.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocketFactory.h"
#include "server/ClientProxy1_3.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <openssl/pem.h>
#include <thread>

class TransportTestArch : public Arch
{
public:
  std::atomic<bool> pauseIo{false};

  int pollSocket(PollEntry entries[], int count, double timeout) override
  {
    if (pauseIo.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return 0;
    }
    return Arch::pollSocket(entries, count, timeout);
  }
};

class ObservedHeartbeatProxy : public ClientProxy1_3
{
public:
  ObservedHeartbeatProxy(deskflow::IStream *stream, IEventQueue *events, int &sent)
      : ClientProxy1_3("synthetic-client", stream, events),
        m_sent(sent)
  {
  }

protected:
  void keepAlive() override
  {
    ++m_sent;
    ClientProxy1_3::keepAlive();
  }

private:
  int &m_sent;
};

class ClipboardTransportTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void initTestCase()
  {
    m_arch.init();
    m_log.setFilter(LogLevel::Level::Info);
    QVERIFY(m_temp.isValid());
    qputenv("XDG_CONFIG_HOME", m_temp.path().toUtf8());
    qputenv("XDG_STATE_HOME", m_temp.path().toUtf8());
#ifdef Q_OS_WIN
    // Windows ignores XDG_CONFIG_HOME and uses system TLS paths unless portable.
    // Claim a new marker beside this test binary; never reuse an existing file.
    m_portableMarker = Settings::portableSettingsFile();
    QVERIFY(QDir().mkpath(QFileInfo(m_portableMarker).absolutePath()));
    QFile marker(m_portableMarker);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      m_portableMarker.clear();
      QSKIP("An existing portable configuration must not be modified by this test");
    }
    marker.close();
#endif
    QDir().mkpath(m_temp.path() + "/Deskflow");
    Settings::setSettingsFile(m_temp.path() + "/Deskflow/Deskflow.conf");
    const auto certificate = m_temp.path() + "/certificate.pem";
    deskflow::generatePemSelfSignedCert(certificate);
    Settings::setValue(Settings::Security::Certificate, certificate);
    auto *bio = BIO_new_file(qPrintable(certificate), "r");
    QVERIFY(bio != nullptr);
    auto *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    QVERIFY(cert != nullptr);
    FingerprintDatabase trusted;
    trusted.addTrusted(deskflow::sslCertFingerprint(cert, QCryptographicHash::Sha256));
    X509_free(cert);
    BIO_free(bio);
    QDir().mkpath(Settings::tlsDir());
    QVERIFY(QFileInfo(Settings::tlsTrustedServersDb()).absoluteFilePath().startsWith(m_temp.path() + "/"));
    QVERIFY(trusted.write(Settings::tlsTrustedServersDb()));
  }

  void cleanupTestCase()
  {
    if (!m_portableMarker.isEmpty()) {
      Settings::save(false);
      QVERIFY(QFile::remove(m_portableMarker));
    }
  }

  void bulkTransferThenControl_data()
  {
    QTest::addColumn<bool>("tls");
    QTest::addColumn<int>("payloadSize");
    QTest::addColumn<bool>("productionHeartbeat");
    QTest::newRow("plain-28MiB") << false << (28 * 1024 * 1024 + 37) << false;
    QTest::newRow("tls-28MiB") << true << (28 * 1024 * 1024 + 37) << false;
    QTest::newRow("tls-48MiB") << true << (48 * 1024 * 1024 + 37) << false;
    QTest::newRow("tls-cap-tail") << true << (1024 * 1024 + 4096 + 37) << false;
    QTest::newRow("tls-two-caps-tail") << true << (2 * (1024 * 1024 + 4096) + 37) << false;
    QTest::newRow("tls-real-server-heartbeat") << true << (28 * 1024 * 1024 + 37) << true;
    // With 16 KiB TLS records and 4 KiB reads, this framing puts DCLP end
    // exactly at the capped read boundary.
    // The following CALV is the only plaintext left in the final TLS record.
    QTest::newRow("tls-complete-clipboard-pending-heartbeat") << true << (1024 * 1024 + 4096 - 97) << false;
  }

  void bulkTransferThenControl()
  {
    QFETCH(bool, tls);
    QFETCH(int, payloadSize);
    QFETCH(bool, productionHeartbeat);
    EventQueue events;
    // Initialize the production queue before manually pumping bounded test phases.
    events.addEvent(Event(EventTypes::Quit));
    QCOMPARE(events.loop(), 0);
    SocketMultiplexer multiplexer;
    const auto resumeIo = qScopeGuard([&] { m_arch.pauseIo = false; });
    TCPSocketFactory factory(&events, &multiplexer);
    // Reserve an OS-selected loopback port, then hand it to the native listener.
    QTcpServer reservation;
    QVERIFY(reservation.listen(QHostAddress::LocalHost, 0));
    const int port = reservation.serverPort();
    reservation.close();
    NetworkAddress address("127.0.0.1", port);
    address.resolve();
    const auto security = tls ? SecurityLevel::Encrypted : SecurityLevel::PlainText;
    std::unique_ptr<IListenSocket> listener(factory.createListen(IArchNetwork::AddressFamily::INet, security));
    std::unique_ptr<IDataSocket> accepted;
    bool clientReady = false;
    bool serverReady = false;
    const auto readyEvent = tls ? EventTypes::DataSocketSecureConnected : EventTypes::DataSocketConnected;
    events.addHandler(EventTypes::ListenSocketConnecting, listener->getEventTarget(), [&](const Event &) {
      accepted = listener->accept();
      if (accepted) {
        if (!tls) {
          serverReady = true;
        } else {
          events.addHandler(EventTypes::ClientListenerAccepted, accepted->getEventTarget(), [&](const Event &) {
            serverReady = true;
          });
        }
      }
    });
    listener->bind(address);
    std::unique_ptr<IDataSocket> client(factory.create(IArchNetwork::AddressFamily::INet, security));
    events.addHandler(readyEvent, client->getEventTarget(), [&](const Event &) { clientReady = true; });
    client->connect(address);
    auto pump = [&](auto condition, int milliseconds) {
      QElapsedTimer timer;
      timer.start();
      while (!condition() && timer.elapsed() < milliseconds) {
        Event event;
        if (events.getEvent(event, 0.01)) {
          events.dispatchEvent(event);
          Event::deleteData(event);
        }
        QCoreApplication::processEvents();
      }
      return condition();
    };
    QVERIFY(pump([&] { return clientReady && serverReady; }, 5000));
    QVERIFY(accepted != nullptr);

    auto sendingOwner = std::make_unique<PacketStreamFilter>(&events, accepted.get(), false);
    auto &sending = *sendingOwner;
    PacketStreamFilter receiving(&events, client.get(), false);
    std::string received;
    ClipboardChunkAssemblyState assembly;
    bool complete = false;
    bool malformed = false;
    int heartbeatsSent = 0;
    int heartbeatsReceived = 0;
    int noopsReceived = 0;
    bool handshake = false;
    std::unique_ptr<ObservedHeartbeatProxy> proxy;
    events.addHandler(EventTypes::StreamInputReady, receiving.getEventTarget(), [&](const Event &) {
      uint8_t code[4];
      while (receiving.read(code, sizeof(code)) != 0) {
        if (memcmp(code, kMsgDClipboard, 4) == 0) {
          ClipboardID id;
          uint32_t sequence;
          const auto result = ClipboardChunk::assemble(&receiving, received, id, sequence, assembly, 64 * 1024 * 1024);
          complete = result == TransferState::Finished || complete;
          malformed = result == TransferState::Error || malformed;
          ProtocolUtil::writef(&receiving, kMsgCNoop);
        } else if (memcmp(code, kMsgCKeepAlive, 4) == 0) {
          ++heartbeatsReceived;
          ProtocolUtil::writef(&receiving, kMsgCKeepAlive);
        } else if (memcmp(code, kMsgQInfo, 4) == 0 && productionHeartbeat) {
          ProtocolUtil::writef(&receiving, kMsgDInfo, 0, 0, 1920, 1080, 0, 0, 0);
        } else if (memcmp(code, kMsgCInfoAck, 4) == 0 && productionHeartbeat) {
          handshake = true;
        } else {
          malformed = true;
        }
      }
    });
    if (productionHeartbeat) {
      proxy = std::make_unique<ObservedHeartbeatProxy>(sendingOwner.release(), &events, heartbeatsSent);
      QVERIFY(pump([&] { return handshake || malformed; }, 3000));
      QVERIFY(!malformed);
    } else {
      events.addHandler(EventTypes::StreamInputReady, sending.getEventTarget(), [&](const Event &) {
        uint8_t code[4];
        while (sending.read(code, sizeof(code)) != 0) {
          if (memcmp(code, kMsgCNoop, 4) == 0) {
            ++noopsReceived;
          } else if (memcmp(code, kMsgCKeepAlive, 4) != 0) {
            malformed = true;
          }
        }
      });
    }
    events.addHandler(EventTypes::ClipboardSending, &sending, [&](const Event &event) {
      auto *chunk = static_cast<ClipboardChunk *>(event.getDataObject());
      ClipboardChunk::send(&sending, chunk);
      if (chunk->m_chunk[5] == ChunkType::DataEnd) {
        ++heartbeatsSent;
        ProtocolUtil::writef(&sending, kMsgCKeepAlive);
        m_arch.pauseIo = false;
        // Let the socket thread fill its input buffer while the consumer is paused.
        QTest::qSleep(200);
      }
    });
    const std::string image(static_cast<size_t>(payloadSize), 'x');
    m_arch.pauseIo = true;
    StreamChunker::sendClipboard(image, image.size(), kClipboardClipboard, 0, &events, &sending);
    // No later record may be needed to wake up delivery of the final clipboard bytes.
    const auto imageDelivered = pump([&] { return (complete && heartbeatsReceived >= 1) || malformed; }, 3000);
    qInfo() << "idle-tail" << imageDelivered << "received" << received.size() << "complete" << complete << "CALV"
            << heartbeatsReceived;
    if (!imageDelivered && !malformed) {
      ProtocolUtil::writef(&sending, kMsgCKeepAlive);
      const auto recovered = pump([&] { return complete && heartbeatsReceived > 0; }, 1000);
      qInfo() << "fresh network record recovered stalled delivery" << recovered;
    }
    QVERIFY(!malformed);
    QVERIFY(imageDelivered);
    QCOMPARE(received, image);
    auto *heartbeat = productionHeartbeat ? nullptr : events.newTimer(0.05, &heartbeatsSent);
    if (heartbeat) {
      events.addHandler(EventTypes::Timer, &heartbeatsSent, [&](const Event &) {
        ++heartbeatsSent;
        ProtocolUtil::writef(&sending, kMsgCKeepAlive);
      });
    }
    const auto delivered = pump([&] { return (complete && heartbeatsReceived >= 4) || malformed; }, 10000);
    events.removeHandler(EventTypes::Timer, &heartbeatsSent);
    if (heartbeat) {
      events.deleteTimer(heartbeat);
    }
    qInfo() << "TLS" << tls << "received" << received.size() << "complete" << complete << "CALV sent/received"
            << heartbeatsSent << heartbeatsReceived << "noops" << noopsReceived;
    QVERIFY(!malformed);
    QVERIFY(delivered);
    QCOMPARE(received, image);
    client->close();
    accepted->close();
    listener->close();
  }

private:
  TransportTestArch m_arch;
  Log m_log;
  QTemporaryDir m_temp;
  QString m_portableMarker;
  // Logging/fingerprint code needs the singleton. Never listen: that would
  // remove the production IPC endpoint and disrupt a running Deskflow instance.
  deskflow::core::ipc::CoreIpcServer m_ipc{this};
};

QTEST_GUILESS_MAIN(ClipboardTransportTests)
#include "ClipboardTransportTests.moc"
