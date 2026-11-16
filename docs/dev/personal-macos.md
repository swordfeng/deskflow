# Personal macOS input-source fix

Branch: `personal/macos-input-source-fix`

Base: pinned upstream commit `c4eb0426f` (`v1.27.0-28`). The first deployed
personal build used `1d530d764` (`v1.26.0-512`), matching the original installation.

This patch follows the main-thread cache approach proposed by
[xx2689 in PR #10327](https://github.com/deskflow/deskflow/pull/10327).
It is a separate implementation rather than a cherry-pick: all production TIS calls,
including key translation, active-group polling, key-map construction, legacy
character conversion and input-source selection, are handled together.

`OSXInputSources` is initialized on the main thread before the core starts.
It publishes immutable snapshots containing copied strings and keyboard-layout
bytes. Selected/enabled input-source notifications refresh the snapshot.
Workers never query TIS and never hold borrowed TIS properties. Layout selection
is queued to the main thread and publishes fresh state before acknowledging.
An unprocessed request times out and is cancelled so shutdown cannot wait forever
for a main thread that has stopped processing events. This timeout cannot interrupt
a native API call that has already started executing.
The singleton intentionally lives until process termination: fatal core paths can
call `exit()` from a worker while the main thread is still using the cache.

The existing ASCII-layout fallback and post-selection delay are preserved.
Input-source IDs are compared by string value instead of CF object addresses.

## Build and test

On Apple Silicon with the command-line developer tools and Homebrew:

```sh
brew install cmake ninja qtbase qttools qtsvg qttranslations openssl@3
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3 \
  -DBUILD_TESTS=ON -DSKIP_BUILD_TESTS=ON
cmake --build build -j 6
ctest --test-dir build/src/unittests --output-on-failure \
  -E '^(OSXClipboardTests|OSXKeyStateTests)$'
build/src/unittests/platform/OSXKeyStateTests mapModifiersFromOSX_OSXMask
cmake --install build --prefix "$PWD/dist"
```

Packaging explicitly deploys Qt frameworks for the embedded `deskflow-core` as
well as the GUI and supplies the QtSvg library directory for split Homebrew Qt
installations. Framework builds copy QtSvg from its resolved framework directory
before deployment because macdeployqt can overlook plugin-only dependencies.
QtSvg is optional, and dylib/static Qt targets never trigger framework-directory
copying. `MacDeployTests` validates all four layouts using isolated CMake fixtures.
Verify the finished bundle with `codesign --verify --deep --strict`
and check both executables for remaining Homebrew library dependencies before
installing it.
Local builds without a signing certificate use ordinary ad-hoc signing. Hardened
runtime signing is enabled only with `APPLE_CODESIGN_DEV`: its library validation
cannot match Team IDs between ad-hoc executables and bundled frameworks.

The excluded native tests inject real keyboard events or modify the clipboard.
The new `OSXInputSourceTests` creates keyboard events in memory without posting
them. Its test-only interposer supplies two synthetic input sources with different
documented `uchr` resources ('a' versus 'q' for the same virtual key), and audits
all six TIS entry points used by production. It captures notification registration
and invokes those callbacks on the main and worker threads without broadcasting
system notifications. It verifies that the audit detects
an intentionally unsafe background call, immutable snapshots survive refreshes,
workers never call TIS during repeated updates, selection publishes its result,
and cancelled requests do not execute later. Selection explicitly starts at the
other source, and the test fails if post-selection publication is removed.
Notification-driven source changes, enabled-source changes and the IME ASCII
fallback are covered. These are simulated notifications; real rapid input-source
switching still needs day-to-day validation. Tests do not change the user's actual
input source.

## Scope and rollback

This targets the TIS concurrency crashes. It does not claim to fix the separate
`Log::print` / `std::mutex::lock` crash observed in another local report.
Passing tests and a short live run do not establish long-term stability; continue
using the normal Mac/client combination and inspect any new crash reports.

Keep the previous application and preferences backup under the local ignored
`build/backups` directory before replacing `/Applications/Deskflow.app`.
To roll back, quit Deskflow, restore the saved application at the same path, and
reopen it. A local ad-hoc signature can require regranting macOS Accessibility
permission for both the application and `deskflow-core`.

Keep new fixes in separate commits. Review upstream changes before rebasing and
pin the upstream revision that has passed validation. No upstream PR,
issue or comment is created by this workflow.

## Windows deployment follow-up

Windows deployment is still required after these review fixes. Use the same
reviewed source commit on both machines, preserve the Windows application and
configuration/TLS material before replacing it, and verify the Windows client
reconnects to the Mac server. Resolve the destination through the connected Paseo
host registry; machine-specific connection details belong in local deployment notes.
