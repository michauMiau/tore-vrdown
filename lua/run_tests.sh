#!/bin/sh
# run_tests.sh -- syntax-check every Lua file, then run both suites.
#
#   sh run_tests.sh
#
# Exits non-zero if anything fails. Requires lua5.1 + luac5.1 (the game runs
# Lua 5.1, so the tests do too).

set -e
cd "$(dirname "$0")"

echo "=== syntax check (luac5.1 -p) ==="
for f in main.lua vrbus.lua vrhaptics.lua vrcamera.lua test/mock_teardown.lua \
         test/test_run.lua test/test_merged.lua; do
  luac5.1 -p "$f"
  echo "  ok   $f"
done

echo
echo "=== build merged single-file variant ==="
python3 merge.py
luac5.1 -p merged/main.lua
echo "  ok   merged/main.lua"

echo
echo "=== suite 1: modular build ==="
lua5.1 test/test_run.lua

echo
echo "=== suite 2: merged build, dofile disabled ==="
lua5.1 test/test_merged.lua

echo
echo "all suites passed"
