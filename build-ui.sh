#!/bin/sh
# Cross-compiles our own userspace programs (home screen, splash, tap) and the
# games into out/. NO_GAMES=1 leaves out Chess and TriKing, NO_QUILL=1 Quill
# (see build-rootfs.sh).
set -e
cd "$(dirname "$0")"
mkdir -p out
# A stack canary in every app: an overflow then stops the app instead of
# corrupting it (musl supports this in static builds).
HARDEN="-fstack-protector-strong"
UI="-Ithird_party/stb -Wno-unused-function -s $HARDEN"
APPS="home splash wifi settings notes sketch browser term books arxiv sshapp dashboard files"
[ "$NO_GAMES" = 1 ] || APPS="$APPS chess"
for p in $APPS; do tools/kcc $UI -o out/$p src/ui/$p.c; done
tools/kcc -s -o out/tap src/test/tap.c
if [ "$NO_QUILL" != 1 ]; then
    tools/kcc $UI -o out/quill src/chat/quill.c
    tools/kcc -s $HARDEN -o out/quill-code src/chat/quillcode.c
fi
[ "$NO_GAMES" = 1 ] || ./build.sh >/dev/null
