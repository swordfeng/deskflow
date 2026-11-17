# Personal release builds

Workflow: `.github/workflows/personal-release.yml`. Pins: `.github/personal/`.
Only this fork's `personal/main` is released; the upstream workflows are not used here.

## What it does

- **Push to `personal/**`**: builds macOS arm64 and Windows x64 **twice each** on
  separate runners, runs the unit tests, and compares the two builds. Nothing is published.
- **Push of an annotated tag `sf-<version>`**: the same, then build provenance is attested and
  an immutable GitHub release is published with:
  - `deskflow-<version>-macos-arm64.zip`: `Deskflow.app`, ad-hoc signed;
  - `deskflow-<version>-win-x64.msi`: the installer;
  - `deskflow-<version>-win-x64-files.zip`: exactly the files the MSI installs;
  - `toolchains.txt`: runner image, compiler, SDK and tool versions of the build.

`<version>` is what CMake derives from `git describe` (for example `1.27.0.45`), and the
tag must match it, point at a commit on `personal/main`, and be annotated. Tags
starting with `v` are left to upstream.

## Pinned inputs

- **Actions**: only GitHub's own (`checkout`, `upload-artifact`, `download-artifact`,
  `attest-build-provenance`), each pinned to a commit.
- **Downloads** (`deps.json`): Qt 6.12.0 archives from the official Qt online repository,
  CMake, Ninja, 7-Zip, and WiX 5.0.2 with its extensions. `fetch.py` checks every SHA-256
  before unpacking anything. WiX is installed from the verified files only: NuGet is
  restricted to a local folder.
- **C/C++ libraries** (`vcpkg/`): vcpkg pinned by commit (which pins every port and its
  source hashes), OpenSSL 3.6.5. There is no binary cache: each build compiles from source.
- **Toolchain**: `macos-15` with Xcode 26.3 (build 17C529); `windows-2022` with MSVC toolset
  14.44 and Windows SDK 10.0.26100.0. The build stops if these aren't what's on the runner.
  The runner images themselves can't be pinned; their versions are recorded in `toolchains.txt`.

## Reproducibility

The two builds of a platform must produce byte-identical files, or nothing is released:

- macOS: the app zip (deterministic, see `repro.py zip`);
- Windows: the zip of the installed files. The MSI itself contains generated GUIDs and
  timestamps and is not compared; instead each build checks that an administrative
  extraction of its MSI (`msiexec /a`) yields exactly the installed files.

What makes it repeatable: source mtimes are set to the commit time
(`SOURCE_DATE_EPOCH`, `QT_RCC_SOURCE_DATE_OVERRIDE`), `ZERO_AR_DATE` for Apple's
archiver, `-ffile-prefix-map` on macOS, `/Brepro` for MSVC (also in vcpkg's triplets).

Limits: hosted runners retire Xcode and MSVC versions over time, so an old tag may stop
being rebuildable on GitHub. The comparison shows that two builds today agree; it does not
protect against a change in the source itself.

## Verifying a release

```sh
gh attestation verify deskflow-<version>-macos-arm64.zip -R swordfeng/deskflow
```

To check a release independently, rebuild the tag with the same pins and compare with
`python3 .github/personal/repro.py diff <yours> <release>`.

## Making a release

```sh
git tag -a sf-<version> -m "Deskflow <version> (personal)"
git push origin sf-<version>
```

Releases are immutable: a broken release gets a new commit and a new tag.
