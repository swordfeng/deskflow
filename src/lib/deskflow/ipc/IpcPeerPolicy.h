/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

namespace deskflow::core::ipc {

/**
 * Who is on the other end of an IPC connection (Windows: the client process's token).
 *
 * The daemon runs as LocalSystem and starts the core, elevated by default, from the settings
 * file a client names. Its pipe has to be reachable by the unelevated GUI, so the pipe itself
 * is open to everyone; this decides which connections are served.
 */
struct IpcPeer
{
  QString userSid;           ///< string SID of the token's user
  quint32 sessionId = 0;     ///< terminal services session of the client process
  quint32 integrityRid = 0;  ///< mandatory label RID (0x2000 medium, 0x3000 high, 0x4000 system)
  bool appContainer = false; ///< token is an AppContainer (sandboxed app)
  bool restricted = false;   ///< token has restricting SIDs (sandboxed process)
};

/// The server's view of the machine when a client connects.
struct IpcPolicyContext
{
  QString serverUserSid;        ///< user of the process running the IPC server
  quint32 consoleSessionId = 0; ///< active console session
  QString consoleUserSid;       ///< user logged on at the console; empty if none or unknown
};

struct IpcPeerVerdict
{
  bool allowed = false;
  QString reason;
};

inline constexpr quint32 kMediumIntegrityRid = 0x2000;
inline const auto kLocalSystemSid = QStringLiteral("S-1-5-18");

/**
 * A client is served if it is
 *  - LocalSystem, or
 *  - running as the same user as the server, or
 *  - the user logged on at the console, from a process in the console session;
 * and in the last two cases not sandboxed (AppContainer or restricted token) and at least
 * medium integrity. Everyone else, e.g. other local accounts such as sandbox users, processes
 * in other sessions, and low-integrity processes, is refused.
 */
inline IpcPeerVerdict evaluateIpcPeer(const IpcPeer &peer, const IpcPolicyContext &context)
{
  if (peer.userSid.isEmpty())
    return {false, QStringLiteral("unknown client identity")};

  if (peer.userSid == kLocalSystemSid)
    return {true, QStringLiteral("local system")};

  if (peer.appContainer || peer.restricted)
    return {false, QStringLiteral("sandboxed client token")};

  if (peer.integrityRid < kMediumIntegrityRid)
    return {false, QStringLiteral("client integrity below medium")};

  if (!context.serverUserSid.isEmpty() && peer.userSid == context.serverUserSid)
    return {true, QStringLiteral("same user as server")};

  if (!context.consoleUserSid.isEmpty() && peer.userSid == context.consoleUserSid &&
      peer.sessionId == context.consoleSessionId)
    return {true, QStringLiteral("console user")};

  return {false, QStringLiteral("not the console user")};
}

} // namespace deskflow::core::ipc
