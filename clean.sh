#!/bin/bash
# clean.sh - Remove build artifacts.
#
# By default this only removes what ./build.sh produces in seconds. The vendored
# dependencies are left alone: rebuilding them from source takes many minutes
# and they almost never need it. Pass --all when they do.
set -e

usage() {
    cat <<'USAGE'
usage: ./clean.sh [options]

  (no options)  remove build/ and .DS_Store files -- rebuilds in seconds
  --vendor      also remove the vendored dependency prefixes (rebuilt from
                source on the next build.sh, which takes several minutes)
  --cache       also remove the downloaded dependency tarballs, forcing a
                re-download as well as a rebuild
  --all         --vendor and --cache together
  -h, --help    show this
USAGE
}

CLEAN_VENDOR=0
CLEAN_CACHE=0

while [ $# -gt 0 ]; do
    case "$1" in
        --vendor) CLEAN_VENDOR=1; shift ;;
        --cache) CLEAN_CACHE=1; shift ;;
        --all) CLEAN_VENDOR=1; CLEAN_CACHE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1"; echo; usage; exit 1 ;;
    esac
done

cd "$(dirname "$0")"

echo "cleaning build artifacts..."

if [ -d "build" ]; then
    rm -rf build
    echo "  removed build/"
fi

find . -name ".DS_Store" -type f -delete
echo "  removed .DS_Store files"

if [ "${CLEAN_VENDOR}" = "1" ]; then
    # Each script knows its own prefix, so ask it rather than hardcoding paths
    # here and having them drift as dependencies are added.
    for script in vendor/build_*.sh vendor/fetch_*.sh; do
        echo "  cleaning $(basename "${script}" .sh | sed 's/^build_//')..."
        "./${script}" --clean > /dev/null
    done
    # Intermediate source trees, shared by every vendor script.
    rm -rf vendor/.build
    echo "  removed vendored dependency prefixes"
fi

if [ "${CLEAN_CACHE}" = "1" ]; then
    rm -rf vendor/.cache
    echo "  removed downloaded dependency tarballs"
fi

echo "clean complete"
if [ "${CLEAN_VENDOR}" = "1" ]; then
    echo "note: the next ./build.sh will rebuild dependencies from source"
fi
