# Personal fork: hardening for privileged installs

Changes on `personal/main` that keep unprivileged code from steering Deskflow, which holds
privileges other programs don't: a SYSTEM token and access to UAC prompts and the logon screen
when the Windows daemon starts the core elevated; the Accessibility grant on macOS.

## IPC connections (Windows)

The daemon and core pipes stay world-accessible (the unelevated GUI must reach the LocalSystem
daemon), but each connection's client process token is checked (`ipc/IpcPeerPolicy.h`,
`ipc/IpcPeerWin.cpp`). Served: LocalSystem, the server's own user, and the user logged on at the
console from a process in the console session. Refused: other accounts (e.g. sandbox users),
other sessions, remote clients, AppContainer and restricted tokens, below medium integrity. A
connection is treated as local only when the client's computer name matches this host; any error
determining it is rejected (fail closed).

## Elevated core launch (Windows)

The daemon starts the elevated core as LocalSystem in the interactive session by duplicating its
*own* SYSTEM token and moving it to the console session, not by copying the token of a process it
finds by the name `winlogon.exe` — another account on the user's desktop could present a process
with that name (`MSWindowsWatchdog.cpp`).

## Managed settings

If `<system dir>/managed/Deskflow.conf` exists and both it and its directory are owned by an
administrator and writable by no one else, it is the only settings file
(`common/ManagedSettings.h`):

| | Windows | macOS |
|---|---|---|
| file | `C:\ProgramData\Deskflow\managed\Deskflow.conf` | `/Library/Deskflow/managed/Deskflow.conf` |
| owner | SYSTEM or Administrators | root |
| writable only by | SYSTEM, Administrators, TrustedInstaller | root (no group/other write) |

In managed mode:

- `--settings`, the user's settings file, portable mode, and the settings file a client names to
  the daemon are ignored; the core refuses to run in a mode other than the managed `core/coreMode`;
- the TLS directory (certificate, `trusted-servers`, `trusted-clients`) is `managed/tls`;
- the Windows daemon starts the core from the managed file at boot, without waiting for a GUI;
- the GUI can't save settings or generate a certificate.

The whole ancestor directory chain, up to the filesystem root, must also be admin/root-owned and
not writable by anyone else, so a writable or foreign-owned parent can't be used to rename or
replace the protected directory. A group/other-writable directory is accepted only if it is
owner-restricted (POSIX sticky bit, e.g. `/Library`). The checks fail closed: a path that can't be
examined (access denied, etc.) counts as untrusted, not absent.

If the file exists but fails the check, the core and the daemon refuse to run instead of falling
back. The application never writes the managed file (`Settings::setValue` ignores non-state keys in
managed mode), so a GUI connection can't make the SYSTEM daemon rewrite it. Changing the server,
peers or certificate is an administrator's job.

Builds configured with `-DDESKFLOW_REQUIRE_MANAGED=ON` (the personal release build) go further:
the core and daemon refuse to run unless trusted managed settings are present, so a deployed
install never falls back to user-controlled settings even if the managed file is removed. The core
additionally refuses to run from a location that isn't admin/root-owned, as defence in depth
against a copy of the signed executable placed where its adjacent libraries could be controlled.

Without managed settings the elevated core on Windows runs whatever settings file the console
user's processes name, including `core/enterCommand`/`exitCommand`; that is why privileged
installs should be managed.

## Loading code from the environment

Core, GUI and daemon remove `QT_PLUGIN_PATH`, `QT_QPA_PLATFORM_PLUGIN_PATH`, `QML(2)_IMPORT_PATH`
and `OPENSSL_CONF`/`_CONF_INCLUDE`/`_MODULES`/`_ENGINES` before anything else
(`common/ProcessHardening.h`), and OpenSSL is initialised without reading a configuration file.
Only bundled plugins are loaded. This matters on macOS, where the app is signed locally with
library validation disabled.

## TLS private key in the keychain (macOS)

With `security/privateKeyKeychain=<service>`, the certificate file holds only the certificate and
the core reads the PEM private key from the generic password `<service>` / `private-key` in the
user's keychains, with keychain UI disabled (`net/KeychainSecret.h`). The item's access control
should list only `deskflow-core`; the keychain identifies it by its designated requirement
(identifier plus signing certificate), so the grant survives updates signed with the same
identity, and other programs get a prompt (to be denied) or nothing.

## Separate private key file

With `security/privateKeyFile=<path>` (any platform) the certificate file holds only the
certificate and the key is read from that file, so the key can be restricted to the core (on
Windows: SYSTEM and Administrators) while the GUI still reads the certificate for the fingerprint.

## Tests

`IpcPeerPolicyTests`, `ManagedSettingsTests` (POSIX and Windows owner/ACL trust rules, ancestor
chain, fail-closed on errors, environment removal). The OS-level pieces (live ACLs and pipe
tokens, keychain access across signed updates) aren't covered by unit tests and are verified at
deployment.
