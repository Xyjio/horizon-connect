#!/bin/sh
# Builds build/HorizonConnect.dll with Zig's C compiler (cross-compiles the Windows DLL on Linux or macOS).
#   ./tools/build.sh             (zig on the PATH)
#   ZIG=/opt/zig/zig ./tools/build.sh
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build"
"${ZIG:-zig}" cc -target x86_64-windows-gnu -shared -O2 -Wall -o "$root/build/HorizonConnect.dll" "$root/plugin/horizonconnect.c" \
  -lws2_32 -lwinhttp -lole32 -luser32 -lshell32 -ladvapi32 -lgdi32
ls -l "$root/build/HorizonConnect.dll"
