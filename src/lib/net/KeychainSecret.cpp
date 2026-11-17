/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "KeychainSecret.h"

#include <iterator>

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace deskflow::net {

#if defined(Q_OS_MACOS)

QByteArray readKeychainSecret(const QString &service, const QString &account, QString &error)
{
  const auto serviceRef = service.toCFString();
  const auto accountRef = account.toCFString();

  const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
  const void *values[] = {kSecClassGenericPassword, serviceRef, accountRef, kCFBooleanTrue, kSecMatchLimitOne};
  const auto query = CFDictionaryCreate(
      kCFAllocatorDefault, keys, values, std::size(keys), &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  );

  // No prompt: access must already be granted to this (signed) program.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  Boolean interactionWas = true;
  SecKeychainGetUserInteractionAllowed(&interactionWas);
  SecKeychainSetUserInteractionAllowed(false);
  CFTypeRef result = nullptr;
  const auto status = SecItemCopyMatching(query, &result);
  SecKeychainSetUserInteractionAllowed(interactionWas);
#pragma clang diagnostic pop

  CFRelease(query);
  CFRelease(serviceRef);
  CFRelease(accountRef);

  if (status != errSecSuccess || result == nullptr || CFGetTypeID(result) != CFDataGetTypeID()) {
    const auto message = SecCopyErrorMessageString(status, nullptr);
    error = QStringLiteral("keychain item %1/%2: %3 (%4)")
                .arg(service, account, message ? QString::fromCFString(message) : QString(), QString::number(status));
    if (message)
      CFRelease(message);
    if (result)
      CFRelease(result);
    return {};
  }

  const auto data = static_cast<CFDataRef>(result);
  QByteArray secret(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
  CFRelease(result);
  return secret;
}

#else

QByteArray readKeychainSecret(const QString &, const QString &, QString &error)
{
  error = QStringLiteral("keychain is only supported on macOS");
  return {};
}

#endif

} // namespace deskflow::net
