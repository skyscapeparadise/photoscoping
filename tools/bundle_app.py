#!/usr/bin/env python3
"""Packages the built binary, its assets and its dylibs into photoscoping.app.

macOS will run the bare executable out of build/ perfectly well, so this is
about being an application rather than a program: an icon in the Dock, a name
in the menu bar, and a directory that can be dragged somewhere else and still
work.

That last part is the actual work. The vendored libraries are dynamic and are
linked by absolute path into this checkout, so a copy of the executable alone
stops working the moment the source tree moves. Every dependency that lives
under vendor/ is copied into the bundle and every reference to it rewritten to
@rpath, walking the graph transitively -- SDL3_image needs SDL3, the FFmpeg
libraries need each other.
"""

from __future__ import annotations

import argparse
import os
import plistlib
import shutil
import subprocess
import sys
from pathlib import Path

# Anything under these prefixes ships with macOS and must not be copied: a
# bundled copy of the system's own libraries is how an app breaks on the next
# OS release.
SYSTEM_PREFIXES = ("/System/", "/usr/lib/", "/Library/Frameworks/")

# The icon sizes an .icns carries. macOS picks by context -- 16 in a list, 32
# in the Finder, 128 in Quick Look, 512 in Get Info -- and each has a @2x twin.
ICON_SIZES = (16, 32, 64, 128, 256, 512, 1024)


def run(cmd: list[str]) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def dependencies(binary: Path) -> list[str]:
    """The install names `binary` links against, its own id excluded."""
    out = run(["otool", "-L", str(binary)]).splitlines()[1:]
    deps = [line.split("(")[0].strip() for line in out if line.strip()]
    # A dylib lists its own id first; a copy of itself is not a dependency.
    own_id = install_id(binary)
    return [d for d in deps if d != own_id]


def install_id(binary: Path) -> str | None:
    out = run(["otool", "-D", str(binary)]).splitlines()
    return out[1].strip() if len(out) > 1 else None


def rpaths(binary: Path) -> list[str]:
    """The LC_RPATH entries, which is how an @rpath dependency is resolved."""
    lines = run(["otool", "-l", str(binary)]).splitlines()
    found, out = False, []
    for line in lines:
        text = line.strip()
        if text.startswith("cmd LC_RPATH"):
            found = True
        elif found and text.startswith("path "):
            out.append(text.split()[1])
            found = False
    return out


def add_rpath(binary: Path, path: str) -> None:
    """Adds an LC_RPATH unless it is already there, which is an error."""
    if path in rpaths(binary):
        return
    run(["install_name_tool", "-add_rpath", path, str(binary)])


def resolve(dep: str, search: list[str]) -> Path | None:
    """Where a dependency actually is, or None for one we do not ship."""
    if dep.startswith(SYSTEM_PREFIXES):
        return None
    if dep.startswith("@rpath/"):
        name = dep[len("@rpath/"):]
        for base in search:
            candidate = Path(base) / name
            if candidate.exists():
                return candidate.resolve()
        return None
    if dep.startswith("@"):
        # @executable_path or @loader_path: already relative to something we
        # are rewriting anyway.
        return None
    path = Path(dep)
    return path.resolve() if path.exists() else None


def collect(binary: Path, frameworks: Path, search: list[str],
            seen: set[str]) -> None:
    """Copies `binary`'s dependencies into the bundle and repoints them."""
    for dep in dependencies(binary):
        source = resolve(dep, search)
        if source is None:
            continue
        name = source.name
        target = frameworks / name
        if name not in seen:
            seen.add(name)
            shutil.copy2(source, target)
            target.chmod(0o755)
            # Its own name, so anything else that links it resolves the same
            # way the executable does.
            run(["install_name_tool", "-id", f"@rpath/{name}", str(target)])
            # A copied library keeps looking for its own dependencies where it
            # was built, so it has to be walked too.
            collect(target, frameworks, search + [str(source.parent)], seen)
        if dep != f"@rpath/{name}":
            run(["install_name_tool", "-change", dep, f"@rpath/{name}",
                 str(binary)])


def write_plist(contents: Path, name: str, identifier: str, version: str) -> None:
    plist = {
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundleExecutable": name,
        "CFBundleIdentifier": identifier,
        "CFBundleIconFile": "icon",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version,
        "CFBundleVersion": version,
        "CFBundleInfoDictionaryVersion": "6.0",
        # Without this the window is drawn at 1x and scaled up, which on a
        # Retina display looks exactly like the blurry mess it is.
        "NSHighResolutionCapable": True,
        "LSMinimumSystemVersion": "12.0",
        "NSSupportsAutomaticGraphicsSwitching": True,
    }
    with (contents / "Info.plist").open("wb") as f:
        plistlib.dump(plist, f)


def build_icon(source: Path, resources: Path) -> None:
    """assets/icon.png to the .icns the Dock and Finder read."""
    icns = resources / "icon.icns"
    # Thirteen sips invocations, on every build, for an image that changes
    # about never.
    if icns.exists() and icns.stat().st_mtime >= source.stat().st_mtime:
        return
    iconset = resources / "icon.iconset"
    if iconset.exists():
        shutil.rmtree(iconset)
    iconset.mkdir(parents=True)
    for size in ICON_SIZES:
        for scale in (1, 2):
            pixels = size * scale
            if pixels > 1024:
                continue
            suffix = "" if scale == 1 else "@2x"
            out = iconset / f"icon_{size}x{size}{suffix}.png"
            run(["sips", "-z", str(pixels), str(pixels), str(source),
                 "--out", str(out)])
    run(["iconutil", "-c", "icns", str(iconset), "-o", str(icns)])
    shutil.rmtree(iconset)


def sync_resource(source: Path, resources: Path) -> None:
    """One resource directory, copied under Resources by its own name.

    The paths in the source are relative -- "assets/intro.mp4",
    "fonts/MonaSans-Regular.ttf" -- and the bundle's Resources directory is
    what the process changes to, so the layout inside it has to match the
    checkout's exactly.

    rsync rather than a fresh copy because this runs on every build and the
    transition videos are most of 62MB.
    """
    target = resources / source.name
    target.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-a", "--delete", f"{source}/", f"{target}/"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--resource", type=Path, action="append",
                        dest="resources", default=[],
                        help="a directory to copy into Resources, by its own "
                             "name; repeatable")
    # Optional until the app has one; macOS shows its generic icon meanwhile.
    parser.add_argument("--icon", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--name", default="photoscoping")
    parser.add_argument("--identifier", default="com.whiteshoal.photoscoping")
    parser.add_argument("--version", default="0.1.0")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"bundle_app: no binary at {args.binary}", file=sys.stderr)
        return 1

    contents = args.out / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    frameworks = contents / "Frameworks"
    for directory in (macos, resources, frameworks):
        directory.mkdir(parents=True, exist_ok=True)

    executable = macos / args.name
    shutil.copy2(args.binary, executable)
    executable.chmod(0o755)

    # Where an @rpath dependency is looked for: the same places the built
    # executable looks, since that is where the vendored libraries are.
    search = rpaths(args.binary) + [str(args.binary.parent)]
    # Cleared first: a stale copy of a library that is no longer linked would
    # ship forever otherwise.
    for old in frameworks.iterdir():
        old.unlink()
    collect(executable, frameworks, search, set())
    add_rpath(executable, "@executable_path/../Frameworks")
    for lib in frameworks.iterdir():
        # A bundled library's own dependencies sit beside it.
        add_rpath(lib, "@loader_path")

    for source in args.resources:
        sync_resource(source, resources)
    if args.icon:
        build_icon(args.icon, resources)
    write_plist(contents, args.name, args.identifier, args.version)

    # Rewriting load commands invalidates whatever signature a library came
    # with, and an unsigned-but-modified binary will not start at all on Apple
    # silicon. Ad-hoc signing is enough for running it here; a distributed
    # build needs a real identity and notarisation.
    for lib in frameworks.iterdir():
        run(["codesign", "--force", "--sign", "-", str(lib)])
    run(["codesign", "--force", "--sign", "-", str(args.out)])

    size = sum(f.stat().st_size for f in args.out.rglob("*") if f.is_file())
    print(f"  packaged {args.out} ({size / 1_000_000:.0f} MB, "
          f"{len(list(frameworks.iterdir()))} bundled libraries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
