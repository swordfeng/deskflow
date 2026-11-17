#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproducibility helpers for the personal release build. Standard library only.

    repro.py source-info          check the release tag, set source mtimes, print env for $GITHUB_ENV
    repro.py zip <dir> <out.zip>  deterministic zip of everything under <dir> (symlinks kept)
    repro.py manifest <dir|zip>   one line per entry: type, mode, sha256 (or link target), path
    repro.py diff <a> <b>         compare two manifests' sources; exit 1 and list differences
"""
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import time
import zipfile
from pathlib import Path

MAIN_BRANCH = "origin/personal/main"


def git(*args: str) -> str:
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout.strip()


def source_info() -> None:
    # Same version scheme as CMakeLists.txt: the latest upstream v* tag plus the commit count.
    desc = git("describe", "--long", "--match", "v*")
    m = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)-(\d+)-g[0-9a-f]+", desc)
    if not m:
        sys.exit(f"unexpected git describe output: {desc}")
    version = ".".join(m.groups())
    epoch = int(git("log", "-1", "--format=%ct"))

    ref = os.environ.get("GITHUB_REF", "")
    if ref.startswith("refs/tags/"):
        tag = ref.removeprefix("refs/tags/")
        if tag != f"sf-{version}":
            sys.exit(f"tag {tag} does not match the version of its commit (expected sf-{version})")
        if git("cat-file", "-t", tag) != "tag":
            sys.exit(f"{tag} is a lightweight tag; release tags must be annotated")
        if subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", MAIN_BRANCH]).returncode != 0:
            sys.exit(f"{tag} is not on {MAIN_BRANCH}")

    # Checkout times differ between runs; anything that records source mtimes (rcc) sees the
    # commit time instead.
    for name in git("ls-files", "-z").split("\0"):
        p = Path(name)
        if name and p.is_file() and not p.is_symlink():
            os.utime(p, (epoch, epoch))

    baseline = json.loads(Path(".github/personal/vcpkg/vcpkg.json").read_text())["builtin-baseline"]
    if not re.fullmatch(r"[0-9a-f]{40}", baseline):
        sys.exit(f"bad vcpkg baseline {baseline!r}")

    print(f"VERSION={version}")
    print(f"VCPKG_COMMIT={baseline}")
    print(f"SOURCE_DATE_EPOCH={epoch}")
    print(f"QT_RCC_SOURCE_DATE_OVERRIDE={epoch}")


def walk(root: Path):
    """Yield (relative posix path, kind, path) for everything under root, sorted, no symlink following."""
    out = []
    for dirpath, dirnames, filenames in os.walk(root):
        d = Path(dirpath)
        for name in list(dirnames):
            p = d / name
            if p.is_symlink():
                dirnames.remove(name)
                out.append((p, "link"))
            else:
                out.append((p, "dir"))
        for name in filenames:
            p = d / name
            out.append((p, "link" if p.is_symlink() else "file"))
    return sorted(((p.relative_to(root).as_posix(), kind, p) for p, kind in out), key=lambda x: x[0])


def file_mode(p: Path) -> int:
    return 0o755 if os.stat(p).st_mode & stat.S_IXUSR else 0o644


def zip_tree(src: str, out: str) -> None:
    root = Path(src)
    epoch = int(os.environ.get("SOURCE_DATE_EPOCH", "315532800"))
    date_time = time.gmtime(max(epoch, 315532800))[:6]  # zip can't store dates before 1980
    with zipfile.ZipFile(out, "w") as z:
        for rel, kind, p in walk(root):
            if kind == "dir":
                zi = zipfile.ZipInfo(rel + "/", date_time)
                zi.external_attr = (stat.S_IFDIR | 0o755) << 16
                data, method = b"", zipfile.ZIP_STORED
            elif kind == "link":
                zi = zipfile.ZipInfo(rel, date_time)
                zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                data, method = os.readlink(p).encode(), zipfile.ZIP_STORED
            else:
                zi = zipfile.ZipInfo(rel, date_time)
                zi.external_attr = (stat.S_IFREG | file_mode(p)) << 16
                data, method = p.read_bytes(), zipfile.ZIP_DEFLATED
            zi.create_system = 3  # unix, so the modes and symlinks above are honoured
            z.writestr(zi, data, compress_type=method, compresslevel=9)


def manifest(src: str) -> list[str]:
    lines = []
    if zipfile.is_zipfile(src) and not Path(src).is_dir():
        with zipfile.ZipFile(src) as z:
            for zi in sorted(z.infolist(), key=lambda i: i.filename):
                mode = zi.external_attr >> 16
                name = zi.filename.rstrip("/")
                if stat.S_ISDIR(mode) or zi.filename.endswith("/"):
                    lines.append(f"dir  -    -  {name}")
                elif stat.S_ISLNK(mode):
                    lines.append(f"link -    {z.read(zi).decode()}  {name}")
                else:
                    digest = hashlib.sha256(z.read(zi)).hexdigest()
                    lines.append(f"file {stat.S_IMODE(mode):o} {digest}  {name}")
    else:
        for rel, kind, p in walk(Path(src)):
            if kind == "dir":
                lines.append(f"dir  -    -  {rel}")
            elif kind == "link":
                lines.append(f"link -    {os.readlink(p)}  {rel}")
            else:
                lines.append(f"file {file_mode(p):o} {hashlib.sha256(p.read_bytes()).hexdigest()}  {rel}")
    return lines


def diff(a: str, b: str) -> None:
    ma, mb = set(manifest(a)), set(manifest(b))
    if ma == mb:
        print(f"identical: {a} == {b}")
        return
    for line in sorted(ma - mb, key=lambda s: s.split("  ", 1)[-1]):
        print(f"- {line}")
    for line in sorted(mb - ma, key=lambda s: s.split("  ", 1)[-1]):
        print(f"+ {line}")
    sys.exit(1)


def main() -> None:
    cmd, *args = sys.argv[1:]
    if cmd == "source-info":
        source_info()
    elif cmd == "zip":
        zip_tree(*args)
    elif cmd == "manifest":
        print("\n".join(manifest(*args)))
    elif cmd == "diff":
        diff(*args)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
