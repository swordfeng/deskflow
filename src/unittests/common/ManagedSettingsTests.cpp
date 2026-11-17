/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ManagedSettingsTests.h"

#include "common/ManagedSettings.h"
#include "common/ProcessHardening.h"

#include <QDir>
#include <QFileInfo>

using namespace deskflow::managed;

namespace {

const auto kSystem = QStringLiteral("S-1-5-18");
const auto kAdmins = QStringLiteral("S-1-5-32-544");
const auto kUsers = QStringLiteral("S-1-5-32-545");
const auto kAuthenticated = QStringLiteral("S-1-5-11");
const auto kCreatorOwner = QStringLiteral("S-1-3-0");
const auto kUser = QStringLiteral("S-1-5-21-1-2-3-1001");
constexpr quint32 kReadExecute = 0x1200a9; // FILE_GENERIC_READ | FILE_EXECUTE
constexpr quint32 kFull = 0x1f01ff;        // FILE_ALL_ACCESS
constexpr quint32 kAddFile = 0x2;          // FILE_ADD_FILE (directory)

PosixEntry posix(quint32 uid, quint32 mode, bool symlink = false)
{
  PosixEntry e;
  e.exists = true;
  e.symlink = symlink;
  e.uid = uid;
  e.mode = mode;
  return e;
}

// A plausible ancestor chain for a root-owned managed directory, ending at a sticky root like
// /Library and then /.
QList<PosixEntry> posixAncestors()
{
  return {posix(0, 0755), posix(0, 01775), posix(0, 0755)}; // dir, /Library (group-writable+sticky), /
}

// What the infra role sets: SYSTEM and Administrators full, Users read.
WinEntry adminOnly()
{
  WinEntry e;
  e.exists = true;
  e.ownerSid = kAdmins;
  e.aces = {{kSystem, kFull, true, false}, {kAdmins, kFull, true, false}, {kUsers, kReadExecute, true, false}};
  return e;
}

QList<WinEntry> winAncestors()
{
  return {adminOnly(), adminOnly(), adminOnly()};
}

} // namespace

void ManagedSettingsTests::posixRootOwnedReadOnlyIsTrusted()
{
  QString why;
  QVERIFY2(posixTrusted(posix(0, 0644), posixAncestors(), why), qPrintable(why));
}

void ManagedSettingsTests::posixUserOwnedIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(501, 0644), posixAncestors(), why));
  QVERIFY(why.contains(QStringLiteral("not owned by root")));
}

void ManagedSettingsTests::posixGroupOrWorldWritableIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(0, 0664), posixAncestors(), why));
  QVERIFY(!posixTrusted(posix(0, 0646), posixAncestors(), why));
}

void ManagedSettingsTests::posixWritableDirectoryIsRefused()
{
  // The file's own directory writable by others (and not sticky): others could replace the file.
  QString why;
  QList<PosixEntry> bad = {posix(0, 0777), posix(0, 0755), posix(0, 0755)};
  QVERIFY(!posixTrusted(posix(0, 0644), bad, why));
  QList<PosixEntry> foreign = {posix(501, 0755), posix(0, 0755)};
  QVERIFY(!posixTrusted(posix(0, 0644), foreign, why));
}

void ManagedSettingsTests::posixWritableAncestorIsRefused()
{
  // A higher ancestor that is world-writable without the sticky bit lets its contents be moved.
  QString why;
  QList<PosixEntry> chain = {posix(0, 0755), posix(0, 0777), posix(0, 0755)};
  QVERIFY(!posixTrusted(posix(0, 0644), chain, why));
  QVERIFY(why.contains(QStringLiteral("ancestor")));
}

void ManagedSettingsTests::posixStickyAncestorIsAllowed()
{
  // /Library is root-owned, group-writable, sticky: a non-owner can't rename the root-owned child.
  QString why;
  QList<PosixEntry> chain = {posix(0, 0755), posix(0, 01777), posix(0, 0755)};
  QVERIFY2(posixTrusted(posix(0, 0644), chain, why), qPrintable(why));
}

void ManagedSettingsTests::posixSymlinkIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(0, 0644, true), posixAncestors(), why));
}

void ManagedSettingsTests::posixErrorIsRefused()
{
  QString why;
  PosixEntry err;
  err.error = true;
  QVERIFY(!posixTrusted(err, posixAncestors(), why));
  QVERIFY(why.contains(QStringLiteral("could not be examined")));
  // An error on an ancestor is equally fatal.
  QList<PosixEntry> chain = {posix(0, 0755), err};
  QVERIFY(!posixTrusted(posix(0, 0644), chain, why));
}

void ManagedSettingsTests::windowsAdminOwnedReadOnlyIsTrusted()
{
  QString why;
  QVERIFY2(windowsTrusted(adminOnly(), winAncestors(), why), qPrintable(why));
  auto systemOwned = adminOnly();
  systemOwned.ownerSid = kSystem;
  QVERIFY2(windowsTrusted(systemOwned, winAncestors(), why), qPrintable(why));
}

void ManagedSettingsTests::windowsUserOwnerIsRefused()
{
  auto file = adminOnly();
  file.ownerSid = kUser;
  QString why;
  QVERIFY(!windowsTrusted(file, winAncestors(), why));
  QVERIFY(why.contains(QStringLiteral("not an administrator")));
}

void ManagedSettingsTests::windowsUserWriteAceIsRefused()
{
  auto file = adminOnly();
  file.aces.append({kUser, kFull, true, false});
  QString why;
  QVERIFY(!windowsTrusted(file, winAncestors(), why));
  QVERIFY(why.contains(kUser));
}

void ManagedSettingsTests::windowsUsersMayCreateFilesInDirectoryIsRefused()
{
  // ProgramData's default: Users may create files; not good enough for an ancestor directory.
  auto dir = adminOnly();
  dir.aces.append({kUsers, kAddFile, true, false});
  QString why;
  QVERIFY(!windowsTrusted(adminOnly(), QList<WinEntry>{dir, adminOnly()}, why));
}

void ManagedSettingsTests::windowsUnprotectedAncestorIsRefused()
{
  // The managed dir is locked down but C:\ProgramData\Deskflow (its parent) is user-owned.
  auto parent = adminOnly();
  parent.ownerSid = kUser;
  QString why;
  QVERIFY(!windowsTrusted(adminOnly(), QList<WinEntry>{adminOnly(), parent, adminOnly()}, why));
  QVERIFY(why.contains(QStringLiteral("ancestor")));
}

void ManagedSettingsTests::windowsInheritOnlyCreatorOwnerIsIgnored()
{
  auto dir = adminOnly();
  dir.aces.append({kCreatorOwner, kFull, true, true});
  QString why;
  QVERIFY2(windowsTrusted(adminOnly(), QList<WinEntry>{dir, adminOnly()}, why), qPrintable(why));
}

void ManagedSettingsTests::windowsDenyAceIsIgnored()
{
  auto file = adminOnly();
  file.aces.append({kAuthenticated, kFull, false, false});
  QString why;
  QVERIFY2(windowsTrusted(file, winAncestors(), why), qPrintable(why));
}

void ManagedSettingsTests::windowsNullDaclIsRefused()
{
  auto file = adminOnly();
  file.hasDacl = false;
  file.aces.clear();
  QString why;
  QVERIFY(!windowsTrusted(file, winAncestors(), why));
}

void ManagedSettingsTests::windowsReparsePointIsRefused()
{
  auto file = adminOnly();
  file.reparsePoint = true;
  QString why;
  QVERIFY(!windowsTrusted(file, winAncestors(), why));
}

void ManagedSettingsTests::windowsErrorIsRefused()
{
  WinEntry err;
  err.error = true;
  QString why;
  QVERIFY(!windowsTrusted(err, winAncestors(), why));
  QVERIFY(!windowsTrusted(adminOnly(), QList<WinEntry>{err}, why));
}

void ManagedSettingsTests::managedFileIsInManagedDirectory()
{
  const QFileInfo info(settingsFile());
  QCOMPARE(info.fileName(), QStringLiteral("Deskflow.conf"));
  QCOMPARE(info.dir().dirName(), QStringLiteral("managed"));
}

void ManagedSettingsTests::unsafeEnvironmentIsRemoved()
{
  for (const auto &name : deskflow::kUnsafeEnvironment)
    qputenv(name.toLatin1().constData(), "/tmp/elsewhere");
  qputenv("DESKFLOW_TEST_KEEP", "1");

  deskflow::hardenProcessEnvironment();

  for (const auto &name : deskflow::kUnsafeEnvironment)
    QVERIFY2(!qEnvironmentVariableIsSet(name.toLatin1().constData()), qPrintable(name));
  QVERIFY(qEnvironmentVariableIsSet("DESKFLOW_TEST_KEEP"));
  QVERIFY(deskflow::kUnsafeEnvironment.contains(QStringLiteral("QT_PLUGIN_PATH")));
  QVERIFY(deskflow::kUnsafeEnvironment.contains(QStringLiteral("OPENSSL_MODULES")));
}

QTEST_MAIN(ManagedSettingsTests)
