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
constexpr quint32 kFull = 0x1f01ff;       // FILE_ALL_ACCESS
constexpr quint32 kAddFile = 0x2;         // FILE_ADD_FILE (directory)

PosixEntry posix(quint32 uid, quint32 mode, bool symlink = false)
{
  return {true, symlink, uid, mode};
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

} // namespace

void ManagedSettingsTests::posixRootOwnedReadOnlyIsTrusted()
{
  QString why;
  QVERIFY2(posixTrusted(posix(0, 0644), posix(0, 0755), why), qPrintable(why));
}

void ManagedSettingsTests::posixUserOwnedIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(501, 0644), posix(0, 0755), why));
  QVERIFY(why.contains(QStringLiteral("not owned by root")));
}

void ManagedSettingsTests::posixGroupOrWorldWritableIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(0, 0664), posix(0, 0755), why));
  QVERIFY(!posixTrusted(posix(0, 0646), posix(0, 0755), why));
}

void ManagedSettingsTests::posixWritableDirectoryIsRefused()
{
  // Others could replace the file through the directory.
  QString why;
  QVERIFY(!posixTrusted(posix(0, 0644), posix(0, 0777), why));
  QVERIFY(!posixTrusted(posix(0, 0644), posix(501, 0755), why));
}

void ManagedSettingsTests::posixSymlinkIsRefused()
{
  QString why;
  QVERIFY(!posixTrusted(posix(0, 0777, true), posix(0, 0755), why));
}

void ManagedSettingsTests::windowsAdminOwnedReadOnlyIsTrusted()
{
  QString why;
  QVERIFY2(windowsTrusted(adminOnly(), adminOnly(), why), qPrintable(why));
  auto systemOwned = adminOnly();
  systemOwned.ownerSid = kSystem;
  QVERIFY2(windowsTrusted(systemOwned, adminOnly(), why), qPrintable(why));
}

void ManagedSettingsTests::windowsUserOwnerIsRefused()
{
  // e.g. a user created the file before the administrator did
  auto file = adminOnly();
  file.ownerSid = kUser;
  QString why;
  QVERIFY(!windowsTrusted(file, adminOnly(), why));
  QVERIFY(why.contains(QStringLiteral("not an administrator")));
}

void ManagedSettingsTests::windowsUserWriteAceIsRefused()
{
  auto file = adminOnly();
  file.aces.append({kUser, kFull, true, false});
  QString why;
  QVERIFY(!windowsTrusted(file, adminOnly(), why));
  QVERIFY(why.contains(kUser));
}

void ManagedSettingsTests::windowsUsersMayCreateFilesInDirectoryIsRefused()
{
  // ProgramData's default: Users may create files; not good enough for the managed directory.
  auto dir = adminOnly();
  dir.aces.append({kUsers, kAddFile, true, false});
  QString why;
  QVERIFY(!windowsTrusted(adminOnly(), dir, why));
  QVERIFY(why.contains(QStringLiteral("directory")));
}

void ManagedSettingsTests::windowsInheritOnlyCreatorOwnerIsIgnored()
{
  auto dir = adminOnly();
  dir.aces.append({kCreatorOwner, kFull, true, true});
  QString why;
  QVERIFY2(windowsTrusted(adminOnly(), dir, why), qPrintable(why));
}

void ManagedSettingsTests::windowsDenyAceIsIgnored()
{
  auto file = adminOnly();
  file.aces.append({kAuthenticated, kFull, false, false});
  QString why;
  QVERIFY2(windowsTrusted(file, adminOnly(), why), qPrintable(why));
}

void ManagedSettingsTests::windowsNullDaclIsRefused()
{
  auto file = adminOnly();
  file.hasDacl = false;
  file.aces.clear();
  QString why;
  QVERIFY(!windowsTrusted(file, adminOnly(), why));
}

void ManagedSettingsTests::windowsReparsePointIsRefused()
{
  auto file = adminOnly();
  file.reparsePoint = true;
  QString why;
  QVERIFY(!windowsTrusted(file, adminOnly(), why));
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
