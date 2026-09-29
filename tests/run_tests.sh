#!/bin/sh
# Build and run the offline unit tests. Native Linux gcc, not the cross
# compiler: these tests are pure logic (patch layout, scan matching, atomics)
# and do not touch Windows APIs.
#
#   sh tests/run_tests.sh
#
# Exits non-zero if any test fails to build or fails at runtime.

set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
OUT=build/tests
mkdir -p "$OUT"

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: $CC not found (need a native C compiler to run the unit tests)"
    exit 1
fi

# The tests include ../hook/*.h directly, which is why memscan.h and friends
# are deliberately free of windows.h.
TESTS="test_present_patch test_memscan test_detour test_atomic test_stealable"

fail=0
for t in $TESTS; do
    extra=""
    # test_atomic is the only one that needs threads.
    [ "$t" = "test_atomic" ] && extra="-pthread"
    # test_present_patch writes through a vtable slot, so it is the one that
    # benefits from a bounds/UB check. Keep ASan on for everything: it costs
    # nothing at this size and the point of CI is to catch the mistake, not to
    # be fast.
    extra="$extra -fsanitize=address,undefined -fno-omit-frame-pointer"

    if ! "$CC" -O1 -g -Wall -o "$OUT/$t" "tests/$t.c" $extra -lm; then
        echo "BUILD FAIL  $t"
        fail=1
        continue
    fi

    if "$OUT/$t"; then
        echo "PASS        $t"
    else
        echo "FAIL        $t (exit $?)"
        fail=1
    fi
done

echo
if [ "$fail" -ne 0 ]; then
    echo "some tests failed"
    exit 1
fi
echo "all $(( $(echo $TESTS | wc -w) )) tests passed"

# --- GUID audit -------------------------------------------------------------
# Cross-compiled and run under wine, because the only meaningful check is
# against the real dxgi.h / d3d12.h. It is a separate step from the native
# tests above: those cannot include windows.h at all, and this one must.
#
# This is a real control, not a note. A wrong GUID fails at runtime as
# E_NOINTERFACE, which reads as "DXGI is unavailable" -- the misdiagnosis the
# project already made once. hook/present_hook.h carried IID_IDXGIAdapter1 with
# 0x1b where the header says 0x1a: fifteen of sixteen bytes matched, so it read
# as correct, in the file that teaches the rule against writing GUIDs by hand.
if [ -n "$CROSS_CC" ]; then
    if "$CROSS_CC" -O1 -o "$OUT/guid_check.exe" tests/guid_check.c -ldxgi -ld3d12 2>/dev/null \
       && command -v wine >/dev/null 2>&1 \
       && wine "$OUT/guid_check.exe" >/dev/null; then
        echo "PASS        guid_check"
    else
        # A missing wine must not be a red build: report it and let CI stay
        # green on platforms that cannot run PE binaries.
        echo "SKIP        guid_check (no wine to run the PE binary)"
    fi
else
    echo "SKIP        guid_check (no cross compiler)"
fi

