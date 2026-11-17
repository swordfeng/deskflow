/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QStringList>
#include <QtGlobal>

namespace deskflow {

/**
 * Environment variables that make Qt or OpenSSL load code or configuration from a path the
 * caller chooses. Deskflow holds privileges other programs don't (Accessibility on macOS, a
 * SYSTEM token on Windows when started by the daemon) and on macOS is signed with library
 * validation disabled, so whoever starts it must not be able to put their own module into it.
 * Only bundled plugins and the built-in OpenSSL defaults are used.
 */
inline const QStringList kUnsafeEnvironment = {
    QStringLiteral("QT_PLUGIN_PATH"),   QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
    QStringLiteral("QML_IMPORT_PATH"),  QStringLiteral("QML2_IMPORT_PATH"),
    QStringLiteral("OPENSSL_CONF"),     QStringLiteral("OPENSSL_CONF_INCLUDE"),
    QStringLiteral("OPENSSL_MODULES"),  QStringLiteral("OPENSSL_ENGINES"),
};

/// Call first thing in main(), before any Qt application object or OpenSSL is initialised.
inline void hardenProcessEnvironment()
{
  for (const auto &name : kUnsafeEnvironment)
    qunsetenv(name.toLatin1().constData());
}

} // namespace deskflow
