#!/bin/sh
# Cross-compiles the freestanding Kindle game TriKing (ARMv7, static, no libc) into out/.
set -e
cd "$(dirname "$0")"
F="--target=armv7a-linux-gnueabi -mfloat-abi=softfp -mfpu=vfpv3-d16 -marm -O2 -ffreestanding -fno-builtin -nostdlib -fno-stack-protector -ffp-contract=off -fno-pic -Wall -Isrc"
mkdir -p out/obj
clang $F -c src/triking.c -o out/obj/triking.o
ld.lld -static -e _start -o out/triking out/obj/triking.o
ls -l out/triking
