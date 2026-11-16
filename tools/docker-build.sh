#!/bin/sh
# Builds the release zip inside Docker, for a computer without the build
# tools (Windows with Docker Desktop, or any Linux or Mac). Needs only Docker.
#
#   tools/docker-build.sh [version]   ->  dist/Inkwell-<version>.zip
#
# The working tree, committed or not, is copied into an alpine:3.24
# container and built there, so the ext3 image gets proper owners, modes and
# symlinks even from a Windows checkout. A Windows drive shows every file as
# 0777, so modes are reset: 644, or 755 for what git marks executable.
# downloads/ lives in the Docker volume "inkwell-downloads", so later builds
# fetch only what is new. Like the release, it fetches only Quill's quick model.
# NO_QUILL=1 and NO_GAMES=1 in the environment build without Quill (and its
# model) or without Chess and TriKing:
#   NO_QUILL=1 NO_GAMES=1 tools/docker-build.sh 1.2.2-pw4
set -e
if [ "$1" = --inside ]; then
    apk add --no-cache git clang llvm lld cmake samurai python3 e2fsprogs e2fsprogs-extra perl-utils curl zip >/dev/null
    mkdir /build
    tar -C /repo --exclude=./.git --exclude=./downloads --exclude=./out --exclude=./dist --exclude=./keys -cf - . | tar -C /build -xf -
    cd /build
    find . -type d -exec chmod 755 {} +
    find . -type f -exec chmod 644 {} +
    git config --global --add safe.directory /repo
    git -C /repo ls-files -s | while read -r mode _ _ path; do
        [ "$mode" = 100755 ] && [ -f "$path" ] && chmod 755 "$path"
    done
    ln -s /cache downloads
    QUICK_ONLY=1 tools/fetch.sh
    tools/release.sh "$2"
    cp dist/* /dist/
    exit 0
fi
cd "$(dirname "$0")/.."
V=${1:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}
here=$(pwd -W 2>/dev/null || pwd) # Git Bash on Windows: Docker wants C:/... paths
mkdir -p dist
MSYS_NO_PATHCONV=1 docker run --rm \
    -e NO_QUILL -e NO_GAMES \
    -v inkwell-downloads:/cache \
    -v "$here:/repo:ro" \
    -v "$here/dist:/dist" \
    alpine:3.24 sh /repo/tools/docker-build.sh --inside "$V"
ls -l dist
