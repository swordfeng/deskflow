/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ManagedSettings.h"

#include "common/Constants.h"
#include "common/Settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <AclAPI.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/stat.h>
#endif

namespace deskflow::managed {

namespace {

bool posixEntryTrusted(const PosixEntry &e, const QString &what, QString &why)
{
  if (!e.exists) {
    why = QStringLiteral("%1 does not exist").arg(what);
    return false;
  }
  if (e.symlink) {
    why = QStringLiteral("%1 is a symbolic link").arg(what);
    return false;
  }
  if (e.uid != 0) {
    why = QStringLiteral("%1 is not owned by root").arg(what);
    return false;
  }
  if ((e.mode & 022) != 0) {
    why = QStringLiteral("%1 is writable by group or others").arg(what);
    return false;
  }
  return true;
}

bool winEntryTrusted(const WinEntry &e, const QString &what, QString &why)
{
  if (!e.exists) {
    why = QStringLiteral("%1 does not exist").arg(what);
    return false;
  }
  if (e.reparsePoint) {
    why = QStringLiteral("%1 is a reparse point").arg(what);
    return false;
  }
  if (!isWinAdminSid(e.ownerSid)) {
    why = QStringLiteral("%1 is owned by %2, not an administrator").arg(what, e.ownerSid);
    return false;
  }
  if (!e.hasDacl) {
    why = QStringLiteral("%1 has no DACL (everyone has full access)").arg(what);
    return false;
  }
  for (const auto &ace : e.aces) {
    if (ace.allow && !ace.inheritOnly && (ace.mask & kWinWriteMask) != 0 && !isWinAdminSid(ace.sid)) {
      why = QStringLiteral("%1 grants write access to %2").arg(what, ace.sid);
      return false;
    }
  }
  return true;
}

#if defined(Q_OS_WIN)
WinEntry readWinEntry(const QString &path)
{
  WinEntry e;
  const auto absolute = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
  const auto native = reinterpret_cast<const wchar_t *>(absolute.utf16());
  const auto attributes = GetFileAttributesW(native);
  if (attributes == INVALID_FILE_ATTRIBUTES)
    return e;
  e.exists = true;
  e.reparsePoint = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (GetNamedSecurityInfoW(
          native, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl,
          nullptr, &sd
      ) != ERROR_SUCCESS) {
    e.ownerSid.clear(); // unreadable: fails the owner check
    return e;
  }

  auto sidString = [](PSID sid) {
    LPWSTR s = nullptr;
    QString result;
    if (sid != nullptr && ConvertSidToStringSidW(sid, &s)) {
      result = QString::fromWCharArray(s);
      LocalFree(s);
    }
    return result;
  };

  e.ownerSid = sidString(owner);
  e.hasDacl = dacl != nullptr;
  if (dacl != nullptr) {
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
      LPVOID raw = nullptr;
      if (!GetAce(dacl, i, &raw))
        continue;
      const auto header = static_cast<ACE_HEADER *>(raw);
      WinAce ace;
      ace.inheritOnly = (header->AceFlags & INHERIT_ONLY_ACE) != 0;
      if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
        const auto a = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        ace.allow = true;
        ace.mask = a->Mask;
        ace.sid = sidString(reinterpret_cast<PSID>(&a->SidStart));
      } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
        const auto a = static_cast<ACCESS_DENIED_ACE *>(raw);
        ace.allow = false;
        ace.mask = a->Mask;
        ace.sid = sidString(reinterpret_cast<PSID>(&a->SidStart));
      } else {
        // Object or callback ACEs: unexpected here, so don't trust the file.
        ace.allow = true;
        ace.mask = kWinWriteMask;
        ace.sid = QStringLiteral("(unsupported ACE type %1)").arg(header->AceType);
      }
      e.aces.append(ace);
    }
  }
  LocalFree(sd);
  return e;
}
#else
PosixEntry readPosixEntry(const QString &path)
{
  PosixEntry e;
  struct stat st = {};
  if (lstat(QFile::encodeName(path).constData(), &st) != 0)
    return e;
  e.exists = true;
  e.symlink = S_ISLNK(st.st_mode);
  e.uid = st.st_uid;
  e.mode = st.st_mode & 07777;
  return e;
}
#endif

} // namespace

bool isWinAdminSid(const QString &sid)
{
  return sid == QStringLiteral("S-1-5-18") || sid == QStringLiteral("S-1-5-32-544") ||
         sid == QStringLiteral("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
}

bool posixTrusted(const PosixEntry &file, const PosixEntry &dir, QString &why)
{
  return posixEntryTrusted(dir, QStringLiteral("managed settings directory"), why) &&
         posixEntryTrusted(file, QStringLiteral("managed settings file"), why);
}

bool windowsTrusted(const WinEntry &file, const WinEntry &dir, QString &why)
{
  return winEntryTrusted(dir, QStringLiteral("managed settings directory"), why) &&
         winEntryTrusted(file, QStringLiteral("managed settings file"), why);
}

QString settingsFile()
{
  return QStringLiteral("%1/managed/%2.conf").arg(Settings::SystemDir, kAppName);
}

State state(QString *why)
{
  const auto file = settingsFile();
  const auto dir = QFileInfo(file).absolutePath();
  QString reason;
  bool trusted = false;
#if defined(Q_OS_WIN)
  const auto fileEntry = readWinEntry(file);
  if (!fileEntry.exists)
    return State::Absent;
  trusted = windowsTrusted(fileEntry, readWinEntry(dir), reason);
#else
  const auto fileEntry = readPosixEntry(file);
  if (!fileEntry.exists)
    return State::Absent;
  trusted = posixTrusted(fileEntry, readPosixEntry(dir), reason);
#endif
  if (why != nullptr)
    *why = reason;
  return trusted ? State::Trusted : State::Untrusted;
}

} // namespace deskflow::managed
