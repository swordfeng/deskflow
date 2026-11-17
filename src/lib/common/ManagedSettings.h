/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QList>
#include <QString>

/**
 * Managed settings: a settings file that only administrators can change.
 *
 * If <system dir>/managed/Deskflow.conf exists (C:\ProgramData\Deskflow\managed on Windows,
 * /Library/Deskflow/managed on macOS, /etc/Deskflow/managed elsewhere), and both the file and
 * its directory are owned by an administrator and not writable by anyone else, it is the only
 * settings file used: the --settings argument, the user's settings file, portable mode and, on
 * Windows, the settings file a client names to the daemon are all ignored. The core and the
 * daemon refuse to run if the file exists but fails the check, rather than silently falling back.
 *
 * This is what makes it safe to let the daemon start the core elevated: who the core connects
 * to, which peers it trusts and which commands it runs are no longer up to unprivileged code.
 */
namespace deskflow::managed {

enum class State
{
  Absent,
  Trusted,
  Untrusted
};

/// Ownership and mode of a file or directory (POSIX).
struct PosixEntry
{
  bool exists = false;
  bool symlink = false;
  quint32 uid = 0;
  quint32 mode = 0;
};

/// One access control entry (Windows).
struct WinAce
{
  QString sid;
  quint32 mask = 0;
  bool allow = true;
  bool inheritOnly = false;
};

/// Owner and DACL of a file or directory (Windows).
struct WinEntry
{
  bool exists = false;
  bool reparsePoint = false;
  QString ownerSid;
  bool hasDacl = true;
  QList<WinAce> aces;
};

/// Access rights that would let a principal change, replace or re-permission a file.
inline constexpr quint32 kWinWriteMask = 0x2 /*FILE_WRITE_DATA, FILE_ADD_FILE*/ | 0x4 /*FILE_APPEND_DATA*/ |
                                         0x10 /*FILE_WRITE_EA*/ | 0x40 /*FILE_DELETE_CHILD*/ |
                                         0x100 /*FILE_WRITE_ATTRIBUTES*/ | 0x10000 /*DELETE*/ |
                                         0x40000 /*WRITE_DAC*/ | 0x80000 /*WRITE_OWNER*/ |
                                         0x10000000 /*GENERIC_ALL*/ | 0x40000000 /*GENERIC_WRITE*/;

/// LocalSystem, Administrators, TrustedInstaller.
bool isWinAdminSid(const QString &sid);

/// Pure checks, separated for tests. `why` gets the reason when the result is false.
bool posixTrusted(const PosixEntry &file, const PosixEntry &dir, QString &why);
bool windowsTrusted(const WinEntry &file, const WinEntry &dir, QString &why);

/// <system dir>/managed/Deskflow.conf
QString settingsFile();

/// Inspects the managed settings file on this system. `why` gets the reason for Untrusted.
State state(QString *why = nullptr);

} // namespace deskflow::managed
