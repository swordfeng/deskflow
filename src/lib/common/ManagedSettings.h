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
  bool error = false; ///< the path could not be examined (e.g. permission denied): never trusted
  bool symlink = false;
  quint32 uid = 0;
  quint32 mode = 0;
  bool sticky() const
  {
    return (mode & 01000) != 0;
  }
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
  bool error = false; ///< the path could not be examined (e.g. access denied): never trusted
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

/// Rights that only let a non-administrator add new entries to a directory (and set its
/// attributes/EAs): FILE_ADD_FILE, FILE_ADD_SUBDIRECTORY, FILE_WRITE_EA, FILE_WRITE_ATTRIBUTES.
/// Without FILE_DELETE_CHILD on the directory or DELETE on an entry, an existing entry can't be
/// renamed, deleted or replaced: the Windows counterpart of the POSIX sticky bit. Windows grants
/// this on C:\ (Authenticated Users) and C:\ProgramData (Users) by default.
inline constexpr quint32 kWinCreateOnlyMask = 0x2 | 0x4 | 0x10 | 0x100;

/// Ancestors at this level and above (0 = the managed directory, 1 = the system dir
/// <SystemDir>, 2 = its parent, e.g. C:\ProgramData) may grant non-administrators create-only
/// rights. The managed directory and the system dir must not: there a user could plant a file the
/// settings refer to before it exists.
inline constexpr int kWinCreateOnlyAncestorLevel = 2;

/// LocalSystem, Administrators, TrustedInstaller.
bool isWinAdminSid(const QString &sid);

/// Pure checks, separated for tests. `why` gets the reason when the result is false. `ancestors`
/// is the chain of directories from the file's own directory up to the filesystem root; every one
/// must be trusted, so a writable or attacker-owned parent can't let the protected file be
/// replaced. A directory that is group/other-writable is still trusted if it is owner-restricted
/// (POSIX sticky bit), since a non-owner then can't rename or delete the root-owned entry in it.
/// On Windows, ancestors from kWinCreateOnlyAncestorLevel up may grant create-only rights
/// (kWinCreateOnlyMask) to non-administrators, for the same reason.
bool posixTrusted(const PosixEntry &file, const QList<PosixEntry> &ancestors, QString &why);
bool windowsTrusted(const WinEntry &file, const QList<WinEntry> &ancestors, QString &why);

/// <system dir>/managed/Deskflow.conf
QString settingsFile();

/// Inspects the managed settings file on this system. `why` gets the reason for Untrusted.
State state(QString *why = nullptr);

/// True if `path` and its immediate directory are owned by root/an administrator and not writable
/// by anyone else (and not a symlink/reparse point). Used in managed mode to refuse running a copy
/// of the executable placed in a user-writable location, whose adjacent libraries an attacker
/// could control. `why` gets the reason when false.
bool pathIsAdminProtected(const QString &path, QString &why);

} // namespace deskflow::managed
