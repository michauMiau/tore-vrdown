#!/bin/sh
# Build teardown_vr.dll and injector.exe from source.
#
# Run from the folder you unpacked this into. Needs the mingw-w64 cross
# compiler, which on Debian/Ubuntu is:
#
#     sudo apt install mingw-w64
#
# On Windows you can install it through MSYS2, UCRT64 environment:
#
#     pacman -S mingw-w64-ucrt-x86_64-gcc
#
# Then just run this script.

set -e

CC=${CC:-x86_64-w64-mingw32-gcc}
# No -fexceptions here, on purpose.
#
# The guarded read used to want __try/__except to catch an access violation on a
# page that another thread freed between the VirtualQuery check and the memcpy.
# That is not available in this toolchain: mingw's excpt.h defines __try1 and
# __except1 but not __try/__except, and gcc 14 posix has no -fseh-exceptions, so
# there is no flag combination that makes it compile. Tested, not assumed.
#
# The read now goes through ReadProcessMemory instead, which probes the range
# in the kernel and returns a short read rather than raising. That closes the
# race by construction and needs no compiler support, which is strictly better
# than catching the fault afterwards.
CFLAGS="-O2 -Wall -Wextra"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: $CC not found."
    echo "On Debian/Ubuntu: sudo apt install mingw-w64"
    echo "On Windows/MSYS2:  pacman -S mingw-w64-ucrt-x86_64-gcc"
    exit 1
fi

mkdir -p build

echo "==> teardown_vr.dll"
# -I third_party: hook/xr_session.h includes <openxr/openxr.h>, and the
# vendored headers live in third_party/openxr/. Without this the build dies
# with "fatal error: openxr/openxr.h: No such file or directory".
"$CC" -shared $CFLAGS -I third_party -o build/teardown_vr.dll \
    hook/teardown_vr.c \
    -lole32 -luser32 \
    -static-libgcc -static-libstdc++ -lm

echo "==> injector.exe"
"$CC" $CFLAGS -o build/injector.exe \
    injector/injector.c \
    -static-libgcc -static-libstdc++

echo "==> stripping (optional, keeps files small)"
if command -v x86_64-w64-mingw32-strip >/dev/null 2>&1; then
    x86_64-w64-mingw32-strip build/teardown_vr.dll build/injector.exe
fi

echo
echo "Done. Files in build/:"
ls -l build/

echo
echo "Verify the result is clean of network capability:"
if command -v x86_64-w64-mingw32-objdump >/dev/null 2>&1; then
    echo -n "  imported DLLs: "
    x86_64-w64-mingw32-objdump -p build/injector.exe \
        | sed -n 's/.*DLL Name: //p' | sort -u | tr '\n' ' '
    echo
fi

echo
echo "Copy build/teardown_vr.dll and build/injector.exe into the game folder."
echo "Defender will flag the injector; see DEFENDER.md for why that is expected."
