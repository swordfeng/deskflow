#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download and unpack the pinned build dependencies of the personal release build.

    fetch.py <mac|win> <dest>

Every file listed for the platform in deps.json is downloaded, its SHA-256 checked against the pin
(any mismatch stops the build before anything is unpacked), then unpacked into <dest>/<into>.
Nothing else is downloaded. Prints what to append to $GITHUB_ENV (QT_ROOT, DEPS_DIR) and appends
the tool directories to $GITHUB_PATH. Standard library only.
"""
import hashlib
import json
import os
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def download(url: str, sha256: str, target: Path) -> None:
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == sha256:
        return
    for attempt in range(1, 5):
        h = hashlib.sha256()
        part = target.with_name(target.name + ".part")
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(part, "wb") as f:
                while chunk := r.read(1 << 20):
                    h.update(chunk)
                    f.write(chunk)
        except OSError as e:
            print(f"download failed ({e}), attempt {attempt}: {url}", file=sys.stderr)
            time.sleep(5 * attempt)
            continue
        got = h.hexdigest()
        if got != sha256:
            part.unlink()
            sys.exit(f"SHA-256 mismatch for {url}: got {got}, pinned {sha256}")
        part.replace(target)
        return
    sys.exit(f"giving up on {url}")


def main() -> None:
    platform, dest = sys.argv[1], Path(sys.argv[2]).resolve()
    entries = json.loads((HERE / "deps.json").read_text())[platform]
    downloads = dest / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)

    # Download and verify everything first, unpack afterwards.
    files = []
    for e in entries:
        target = downloads / e["url"].rsplit("/", 1)[1]
        download(e["url"], e["sha256"], target)
        print(f"verified {e['sha256']}  {target.name}", file=sys.stderr)
        files.append((e, target))

    sevenzip = dest / "7zip" / ("7zr.exe" if platform == "win" else "7zz")
    paths = []
    for e, f in files:
        into = dest / e["into"]
        into.mkdir(parents=True, exist_ok=True)
        kind = e["extract"]
        if kind == "none":
            (into / f.name).write_bytes(f.read_bytes())
        elif kind == "zip":
            with zipfile.ZipFile(f) as z:
                z.extractall(into)
        elif kind == "tar":
            with tarfile.open(f) as t:
                t.extractall(into, filter="data")
        elif kind == "7z":
            subprocess.run([str(sevenzip), "x", "-y", "-bso0", "-bsp0", f"-o{into}", str(f)], check=True)
        else:
            sys.exit(f"unknown extract kind {kind}")
        for name in e.get("executable", []):
            p = into / name
            p.chmod(p.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        if "path" in e:
            paths.append((into / e["path"]).resolve())

    # The archives unpack flat (bin/, lib/, plugins/ ...) into the same directory.
    configs = sorted((dest / "qt").rglob("lib/cmake/Qt6/Qt6Config.cmake"))
    if len(configs) != 1:
        sys.exit(f"expected exactly one Qt installation, found {configs}")
    qt_root = configs[0].parents[3]
    paths.append(qt_root / "bin")

    if gh_path := os.environ.get("GITHUB_PATH"):
        with open(gh_path, "a") as f:
            for p in paths:
                f.write(f"{p}\n")
    print(f"QT_ROOT={qt_root.as_posix()}")
    print(f"DEPS_DIR={dest.as_posix()}")


if __name__ == "__main__":
    main()
