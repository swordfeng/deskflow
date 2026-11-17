/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class IpcPeerPolicyTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void localSystemIsAllowed();
  void consoleUserIsAllowed();
  void elevatedConsoleUserIsAllowed();
  void sameUserAsServerIsAllowed();
  void otherAccountInConsoleSessionIsRefused();
  void consoleUserInOtherSessionIsRefused();
  void lowIntegrityIsRefused();
  void appContainerIsRefused();
  void restrictedTokenIsRefused();
  void unknownIdentityIsRefused();
  void withoutConsoleUserOnlySystemAndSameUser();
};
