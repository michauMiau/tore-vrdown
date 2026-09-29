#!/bin/sh
# Test the VR input layer against a mock of the Teardown Lua API.
#
# WHY A MOCK
#
# The only honest way to test this while the game is not being driven by a
# human is to run the real file against a stand-in for the engine API. The mock
# is deliberately small and only contains the functions vrinput.lua actually
# calls. If vrinput.lua starts calling something new, the mock makes that a
# hard failure rather than a silent pass.
#
# The mock also records what was asked of it, so the assertions can check
# behaviour (did it hold the right action?) and not just "did it not crash".
#
#   sh lua/run_input_tests.sh

set -e
cd "$(dirname "$0")"

LUA=${LUA:-lua5.4}
command -v "$LUA" >/dev/null 2>&1 || LUA=lua
command -v "$LUA" >/dev/null 2>&1 || { echo "no lua interpreter found"; exit 1; }

echo "=== interpreter: $LUA ($($LUA -v 2>&1 | head -1)) ==="
echo
$LUA input_tests.lua
echo
echo "=== all input-layer tests passed ==="
