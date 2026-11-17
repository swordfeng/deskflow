/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcPeerPolicyTests.h"

#include "deskflow/ipc/IpcPeerPolicy.h"

using namespace deskflow::core::ipc;

namespace {

const auto kUser = QStringLiteral("S-1-5-21-1-2-3-1001");
const auto kSandbox = QStringLiteral("S-1-5-21-1-2-3-1002");
constexpr quint32 kConsole = 1;
constexpr quint32 kHigh = 0x3000;
constexpr quint32 kLow = 0x1000;

// The daemon: LocalSystem, with the user logged on at the console in session 1.
IpcPolicyContext daemonContext()
{
  return {kLocalSystemSid, kConsole, kUser};
}

IpcPeer peer(const QString &sid, quint32 session = kConsole, quint32 integrity = kMediumIntegrityRid)
{
  IpcPeer p;
  p.userSid = sid;
  p.sessionId = session;
  p.integrityRid = integrity;
  return p;
}

} // namespace

void IpcPeerPolicyTests::localSystemIsAllowed()
{
  QVERIFY(evaluateIpcPeer(peer(kLocalSystemSid, 0, 0x4000), daemonContext()).allowed);
}

void IpcPeerPolicyTests::consoleUserIsAllowed()
{
  QVERIFY(evaluateIpcPeer(peer(kUser), daemonContext()).allowed);
}

void IpcPeerPolicyTests::elevatedConsoleUserIsAllowed()
{
  QVERIFY(evaluateIpcPeer(peer(kUser, kConsole, kHigh), daemonContext()).allowed);
}

void IpcPeerPolicyTests::sameUserAsServerIsAllowed()
{
  // A core started unelevated runs as the user; its own GUI talks to it.
  const IpcPolicyContext context{kUser, kConsole, {}};
  QVERIFY(evaluateIpcPeer(peer(kUser, 2), context).allowed);
}

void IpcPeerPolicyTests::otherAccountInConsoleSessionIsRefused()
{
  // e.g. a sandbox account whose processes run on the user's desktop
  const auto verdict = evaluateIpcPeer(peer(kSandbox), daemonContext());
  QVERIFY(!verdict.allowed);
  QCOMPARE(verdict.reason, QStringLiteral("not the console user"));
}

void IpcPeerPolicyTests::consoleUserInOtherSessionIsRefused()
{
  QVERIFY(!evaluateIpcPeer(peer(kUser, 2), daemonContext()).allowed);
}

void IpcPeerPolicyTests::lowIntegrityIsRefused()
{
  QVERIFY(!evaluateIpcPeer(peer(kUser, kConsole, kLow), daemonContext()).allowed);
}

void IpcPeerPolicyTests::appContainerIsRefused()
{
  auto p = peer(kUser);
  p.appContainer = true;
  QVERIFY(!evaluateIpcPeer(p, daemonContext()).allowed);
}

void IpcPeerPolicyTests::restrictedTokenIsRefused()
{
  auto p = peer(kUser);
  p.restricted = true;
  QVERIFY(!evaluateIpcPeer(p, daemonContext()).allowed);
}

void IpcPeerPolicyTests::unknownIdentityIsRefused()
{
  QVERIFY(!evaluateIpcPeer(peer({}), daemonContext()).allowed);
}

void IpcPeerPolicyTests::withoutConsoleUserOnlySystemAndSameUser()
{
  // Nobody logged on (or the console user unknown): the daemon serves only LocalSystem.
  const IpcPolicyContext context{kLocalSystemSid, kConsole, {}};
  QVERIFY(evaluateIpcPeer(peer(kLocalSystemSid, 0, 0x4000), context).allowed);
  QVERIFY(!evaluateIpcPeer(peer(kUser), context).allowed);
}

QTEST_MAIN(IpcPeerPolicyTests)
