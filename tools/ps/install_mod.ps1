$ErrorActionPreference = 'Continue'

# Installer for the Teardown VR mod: game-side Lua only.
# The native Steam Input shim is a separate artefact and is not installed here.
#
# Idempotent by construction: every edited game file is rebuilt from a pristine
# copy kept beside it as <name>.tdvrorig, and every edit is bracketed by
# markers. Running this twice leaves byte-identical results. Deleting the
# .tdvrorig files reverts everything.

$game = if ($args.Count -ge 1) { $args[0] } else { 'D:\SteamLibrary\steamapps\common\Teardown' }
$mod  = "$game\mods\teardown_vr"

'=== Teardown VR mod installer ==='
'game: ' + $game
if (-not (Test-Path "$game\data\script")) { 'ERROR: not a Teardown install (' + $game + ')'; exit 1 }

# --- 1. mod files ----------------------------------------------------------
New-Item -ItemType Directory -Force -Path $mod | Out-Null
foreach ($f in 'main.lua', 'tdvr_boot.lua', 'info.txt', 'MERGED_BUILD.txt') {
  $src = "C:\tdvr\payload\$f"
  if (Test-Path $src) { Copy-Item $src "$mod\$f" -Force; '  + ' + $f }
  else { '  ! missing payload: ' + $f }
}

# --- 2. common.lua: one-shot bootstrap -------------------------------------
# loadfile with an ABSOLUTE path. Relative paths return nil, and dofile() runs
# nothing while reporting success, so neither may be used here.
$common = "$game\data\script\common.lua"
$cbak   = "$common.tdvrorig"
if (-not (Test-Path $cbak)) { Copy-Item $common $cbak; '  created pristine common.lua' }
$src = Get-Content $cbak -Raw
# The block must end with a newline. Set-Content -NoNewline once glued the
# closing marker to the first line of the original file, and since the marker is
# a line comment that silently swallowed `function clamp(...)` -- the whole
# shipped helper library stopped parsing and every TDVR key went ABSENT. That
# failure looks exactly like "the bootstrap never ran", which is why it survived
# so many runs.
$block = @'

-- ==== TDVR (teardown VR) bootstrap ====
do
  local _f = loadfile("D:/SteamLibrary/steamapps/common/Teardown/mods/teardown_vr/tdvr_boot.lua")
  if _f then _f() end
end
-- ==== end TDVR bootstrap ====

'@
if ($src -notmatch '==== TDVR') { $src = $block + $src; Set-Content $common $src }
# Always rebuild from the pristine copy, so a re-run cannot stack blocks. The
# message reports the resulting state, not whether this pass did the work.
'  common.lua = ' + (Get-Item $common).Length + ' bytes, ' +
  ([regex]::Matches((Get-Content $common -Raw), '==== TDVR')).Count + ' marker(s)'

# --- 3. ui/menu.lua: the frame host ---------------------------------------
# common.lua defines no init/tick/draw -- it is a helper library. The engine
# loads data/ui/menu.lua directly (nothing #includes it) and that file owns the
# bare lifecycle, so this is the only place frames come from.
$menu = "$game\data\ui\menu.lua"
$mbak = "$menu.tdvrorig"
if (-not (Test-Path $mbak)) { Copy-Item $menu $mbak; '  created pristine menu.lua' }
$src = Get-Content $mbak -Raw
if ($src -notmatch '==== TDVR') {
  # No -NoNewline here for the same reason as common.lua: a here-string that
  # replaces "function tick(dt)" ends with a newline, and dropping it welds the
  # block onto the host's next line.
  $src = $src.Replace("function tick(dt)", @"
function tick(dt)
  -- ==== TDVR (teardown VR) ====
  if _G.TDVR then pcall(_G.TDVR.tick, dt) end
  -- ==== end TDVR ====
"@)
  $src = $src.Replace("function draw()", @"
function draw()
  -- ==== TDVR (teardown VR) ====
  if _G.TDVR then pcall(_G.TDVR.draw) end
  -- ==== end TDVR ====
"@)
  Set-Content $menu $src
}
# Always rebuild from the pristine copy, so a re-run cannot stack blocks. Two
# markers are the correct end state: one in tick, one in draw.
'  menu.lua = ' + (Get-Item $menu).Length + ' bytes, ' +
  ([regex]::Matches((Get-Content $menu -Raw), '==== TDVR')).Count + ' marker(s)'

'=== done ==='
'  rollback: delete common.lua.tdvrorig and menu.lua.tdvrorig, then copy each back'
