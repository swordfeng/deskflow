/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QByteArray>
#include <QString>

namespace deskflow::net {

/// Account name under which the TLS private key is stored (service: Settings::Security::PrivateKeyKeychain).
inline const auto kKeychainPrivateKeyAccount = QStringLiteral("private-key");

/**
 * macOS: read a generic password from the user's keychains without any user interaction.
 *
 * The item's access control lists the applications that may read it; for the TLS key that is
 * deskflow-core, identified by its signature, so other programs can't read it without the user
 * approving a prompt. Here the prompt is suppressed: if access isn't already granted, this fails.
 * Returns an empty array and sets `error` on failure. Not available on other platforms.
 */
QByteArray readKeychainSecret(const QString &service, const QString &account, QString &error);

} // namespace deskflow::net
