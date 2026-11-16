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

## Windows deployment

Windows deployment completed on 2026-11-16 from commit `f5872be93`: GUI, core and
daemon are version **1.27.0.39**. The Mac remains on **1.27.0.37** (`4c0e6138f`);
the subsequent production changes affect only Windows. The Windows client
reconnected to the Mac server over TLS after the installed directory was replaced
and its existing automatic LocalSystem service restarted. TLS identity/trust files
were verified unchanged. Preserve the application, user/system configuration and
TLS material before future replacements. Machine-specific connection details,
backup locations, package checksums and process IDs belong in ignored deployment
records, not this document.

The full Windows build used MSVC 19.44, Qt 6.12.0 and OpenSSL 3.6.5 (vcpkg commit
`8f363bdc11359ede4bc51e5eb32a2617538049c8`). Use the aqt installer revision pinned
in CI for Qt's newer repository layout. The SDK must include qtdeclarative as well
as qtbase, qttools, qtsvg and qttranslations: `lupdate.exe` depends on `Qt6Qml.dll`.
Build with `BUILD_TESTS=ON`, `SKIP_BUILD_TESTS=ON` and
`CMAKE_COMPILE_WARNING_AS_ERROR=ON`. All **29** non-invasive Windows CTest targets
passed; only `MSWindowsClipboardTests`, which changes the live clipboard, was
excluded. The installed staging directory ran `deskflow-core --version` with SDK
paths removed from PATH. All 21 staged PE files had their DLL dependencies
available, and all 68 staged files were hashed and verified during replacement.

The first full test run also exposed an existing Windows socket-option bug.
The native Winsock probe returned a one-byte `TCP_NODELAY` value, leaving the
remaining bytes of its BOOL buffer untouched. Initializing that buffer fixes the
uninitialized read and makes the existing return-previous-value test pass.
This is isolated in commit `f5872be93`.

### Windows clipboard bitmap safety

The Windows bitmap converter now validates incoming wire DIBs before publishing
them, and native HGLOBAL DIBs before reading their pixels or passing them to GDI.
INFOHEADER bitfield masks are external; V4/V5 masks are embedded and must not
advance the pixel pointer a second time. Validation covers header lengths,
planes/depth/compression, signed dimensions, padded scanlines, palette and mask
ranges, declared image sizes, packed V5 profile ranges, and 32-bit wire/output
size limits. HGLOBAL sizes retain SIZE_T precision. Locks and GDI resources are
released on every return and on C++ allocation exceptions; failed GDI conversions
return no image. The existing repair for an INFOHEADER-sized Mac payload that
incorrectly declares a V5 header remains supported.

This path supports uncompressed RGB and bitfield DIBs. Unsupported headers and
RLE/JPEG/PNG-compressed DIBs are rejected with a warning; PNG transport is a
separate change and is not added by this patch.

`MSWindowsClipboardBitmapTests` is a separate Windows test target that never
opens or changes the system clipboard. Its nine cases cover actual pixel colors
for INFO/V4/V5 bitfields in both orientations, healthy/legacy Mac publication,
24/32-bit RGB padding, truncated headers/masks/palettes/pixels, invalid dimensions
and metadata, V5 profile bounds, indexed/16-bit conversion, a 4096x2160 image,
and repeated conversion without GDI object leaks. The legacy repair cannot
distinguish its historical 40-byte-header payload from an identically sized V5
truncation; the truncation test explicitly preserves that compatibility exception.

Validation on Windows used Clang 22.1.7, the real Windows SDK/GDI and
AddressSanitizer, compiling the production converter and test source in an
isolated harness. Only the Qt-dependent logger was replaced with a no-op shim.
The subsequent complete Qt Windows build and deployment are recorded above;
the bitmap test also passed there using the real logger.
All nine cases passed without sanitizer reports. Restoring the erroneous V4/V5
12-byte pixel offset made the pixel-color and large-image cases fail; restoring
the fix passed all nine again. Mac build and 30 non-invasive tests also passed.
These results establish the converter defect and its regression coverage, not
the cause of any specific historical client crash without a matching stack.
