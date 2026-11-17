/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ManagedSettingsTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void posixRootOwnedReadOnlyIsTrusted();
  void posixUserOwnedIsRefused();
  void posixGroupOrWorldWritableIsRefused();
  void posixWritableDirectoryIsRefused();
  void posixSymlinkIsRefused();
  void windowsAdminOwnedReadOnlyIsTrusted();
  void windowsUserOwnerIsRefused();
  void windowsUserWriteAceIsRefused();
  void windowsUsersMayCreateFilesInDirectoryIsRefused();
  void windowsInheritOnlyCreatorOwnerIsIgnored();
  void windowsDenyAceIsIgnored();
  void windowsNullDaclIsRefused();
  void windowsReparsePointIsRefused();
  void managedFileIsInManagedDirectory();
  void unsafeEnvironmentIsRemoved();
};
