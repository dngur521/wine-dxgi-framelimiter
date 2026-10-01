#!/bin/sh
# Build the DXMT dxgi proxy (and the test harness) with mingw-w64.
set -e
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
mkdir -p build
$CC -O2 -Wall -shared -o build/dxgi.dll src/dxgi_proxy.c src/dxgi.def \
    -static-libgcc -Wl,--enable-stdcall-fixup
$CC -O2 -Wall -o build/fltest.exe test/fltest.c -ld3d11 -ldxgi -static-libgcc
echo "built: build/dxgi.dll build/fltest.exe"
