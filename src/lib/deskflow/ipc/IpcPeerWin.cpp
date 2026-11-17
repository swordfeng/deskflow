/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcPeerWin.h"

#include <QLocalSocket>

#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WtsApi32.h>
#include <sddl.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace deskflow::core::ipc {

namespace {

struct ScopedHandle
{
  HANDLE handle = nullptr;
  ScopedHandle() = default;
  explicit ScopedHandle(HANDLE h) : handle(h)
  {
  }
  ScopedHandle(const ScopedHandle &) = delete;
  ScopedHandle &operator=(const ScopedHandle &) = delete;
  ~ScopedHandle()
  {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
};

std::vector<BYTE> tokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS type)
{
  DWORD size = 0;
  GetTokenInformation(token, type, nullptr, 0, &size);
  if (size == 0)
    return {};
  std::vector<BYTE> buffer(size);
  if (!GetTokenInformation(token, type, buffer.data(), size, &size))
    return {};
  return buffer;
}

QString tokenUserSid(HANDLE token)
{
  const auto buffer = tokenInformation(token, TokenUser);
  if (buffer.empty())
    return {};
  LPWSTR sidString = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER *>(buffer.data())->User.Sid, &sidString))
    return {};
  const auto result = QString::fromWCharArray(sidString);
  LocalFree(sidString);
  return result;
}

quint32 tokenIntegrityRid(HANDLE token)
{
  const auto buffer = tokenInformation(token, TokenIntegrityLevel);
  if (buffer.empty())
    return 0;
  const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(buffer.data())->Label.Sid;
  if (!IsValidSid(sid))
    return 0;
  const auto count = *GetSidSubAuthorityCount(sid);
  if (count == 0)
    return 0;
  return *GetSidSubAuthority(sid, count - 1);
}

bool tokenIsAppContainer(HANDLE token)
{
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (!GetTokenInformation(token, TokenIsAppContainer, &value, size, &size))
    return true; // unknown: treat as sandboxed
  return value != 0;
}

} // namespace

std::optional<IpcPeer> queryIpcPeer(const QLocalSocket *socket, QString &error)
{
  const auto pipe = reinterpret_cast<HANDLE>(socket->socketDescriptor());
  if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
    error = QStringLiteral("no pipe handle");
    return std::nullopt;
  }

  // Only local clients: for a remote one (named pipes are reachable over SMB) the client process
  // id and session would refer to another machine. Accept only when the client's computer name can
  // be read and equals this computer's; any failure or mismatch is rejected (fail closed).
  wchar_t clientComputer[MAX_COMPUTERNAME_LENGTH + 1] = {};
  if (!GetNamedPipeClientComputerNameW(pipe, clientComputer, sizeof(clientComputer))) {
    error = QStringLiteral("cannot determine client computer (error %1)").arg(GetLastError());
    return std::nullopt;
  }
  wchar_t localComputer[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD localLen = ARRAYSIZE(localComputer);
  if (!GetComputerNameW(localComputer, &localLen)) {
    error = QStringLiteral("cannot determine local computer (error %1)").arg(GetLastError());
    return std::nullopt;
  }
  if (_wcsicmp(clientComputer, localComputer) != 0) {
    error = QStringLiteral("remote client");
    return std::nullopt;
  }

  ULONG pid = 0;
  ULONG session = 0;
  if (!GetNamedPipeClientProcessId(pipe, &pid) || !GetNamedPipeClientSessionId(pipe, &session)) {
    error = QStringLiteral("cannot identify client process (error %1)").arg(GetLastError());
    return std::nullopt;
  }

  // The client holds the connection open, so its pid can't have been reused.
  ScopedHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (process.handle == nullptr) {
    error = QStringLiteral("cannot open client process %1 (error %2)").arg(pid).arg(GetLastError());
    return std::nullopt;
  }
  ScopedHandle token;
  if (!OpenProcessToken(process.handle, TOKEN_QUERY, &token.handle)) {
    error = QStringLiteral("cannot open token of client process %1 (error %2)").arg(pid).arg(GetLastError());
    return std::nullopt;
  }

  IpcPeer peer;
  peer.userSid = tokenUserSid(token.handle);
  peer.sessionId = session;
  peer.integrityRid = tokenIntegrityRid(token.handle);
  peer.appContainer = tokenIsAppContainer(token.handle);
  peer.restricted = IsTokenRestricted(token.handle) != FALSE;
  return peer;
}

IpcPolicyContext currentIpcPolicyContext()
{
  IpcPolicyContext context;

  ScopedHandle self;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &self.handle))
    context.serverUserSid = tokenUserSid(self.handle);

  context.consoleSessionId = WTSGetActiveConsoleSessionId();
  if (context.consoleSessionId != 0xFFFFFFFF) {
    // Needs SeTcbPrivilege, i.e. the daemon or a core started by it; otherwise only the
    // same-user rule applies.
    ScopedHandle user;
    if (WTSQueryUserToken(context.consoleSessionId, &user.handle))
      context.consoleUserSid = tokenUserSid(user.handle);
  }
  return context;
}

} // namespace deskflow::core::ipc
