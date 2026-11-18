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
  void posixWritableAncestorIsRefused();
  void posixStickyAncestorIsAllowed();
  void posixSymlinkIsRefused();
  void posixErrorIsRefused();
  void windowsAdminOwnedReadOnlyIsTrusted();
  void windowsUserOwnerIsRefused();
  void windowsUserWriteAceIsRefused();
  void windowsUsersMayCreateFilesInDirectoryIsRefused();
  void windowsUnprotectedAncestorIsRefused();
  void windowsDefaultSystemAncestorsAreTrusted();
  void windowsCreateOnlyInSystemDirIsRefused();
  void windowsDeleteChildInHighAncestorIsRefused();
  void windowsInheritOnlyCreatorOwnerIsIgnored();
  void windowsDenyAceIsIgnored();
  void windowsNullDaclIsRefused();
  void windowsReparsePointIsRefused();
  void windowsErrorIsRefused();
  void managedFileIsInManagedDirectory();
  void unsafeEnvironmentIsRemoved();
};
