/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "IpcPeerPolicy.h"

#include <optional>

class QLocalSocket;

namespace deskflow::core::ipc {

/// Identity of the process at the other end of a connected local socket (named pipe).
/// Returns nothing, with a reason in `error`, if it can't be established (e.g. a remote client).
std::optional<IpcPeer> queryIpcPeer(const QLocalSocket *socket, QString &error);

/// The running server's own user and the console session's user (needs LocalSystem).
IpcPolicyContext currentIpcPolicyContext();

} // namespace deskflow::core::ipc
