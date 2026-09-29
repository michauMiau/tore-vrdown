--[[---------------------------------------------------------------------------
  campaign autopilot

    The user cannot reach the keyboard (no controller attached, Steam Link too
    slow), so the mod has to get into a level by itself. The engine exposes the
    whole campaign path, measured from the shipped sources:

      data/missions.lua:5   gMissions = {}          -- global
      data/missions.lua     41 entries, each with .file and .layers
      data/game.lua:327     function saveAndStartLevel(mission, title, image,
                                                path, layers, passThrough)
                             -- global, sets game.lobby.level.* then StartLevel

    So the mod calls saveAndStartLevel itself once the menu is up. That is the
    real path the menu uses -- no input is synthesized, and nothing here depends
    on a keypress working.

    Fires late and once, from tick(), because at init() the mission table is
    not guaranteed to be populated yet and a failed StartLevel leaves the game in
    the menu with no way back.
-------------------------------------------------------------------------------]]
TDVR.campaign = {
	armed   = false,
	tries   = 0,
	done    = false,
	mission = nil,
}

-- Probe order: the first level of the campaign, then anything that parses.
-- "mall_intro" is the opening mission (data/missions.lua), so it is the
-- cheapest level that still exercises a real render loop, tick and draw.
local CANDIDATES = {
	"mall_intro",
	"mall_foodcourt",
}

local function pickMission()
	if not TDVR.campaign.mission then
		for _, id in ipairs(CANDIDATES) do
			if gMissions and gMissions[id] then
				TDVR.campaign.mission = id
				return gMissions[id]
			end
		end
	end
	local m = TDVR.campaign.mission
	return m and gMissions and gMissions[m] or nil
end

function TDVR.campaignPoll()
	if TDVR.campaign.done then return end
	TDVR.campaign.tries = TDVR.campaign.tries + 1

	-- Wait for the menu to have ticked a while, so the level list is built and
	-- the engine is out of its loading state. 90 polls is a few seconds.
	if TDVR.ticked < 90 then return end
	if type(_G.saveAndStartLevel) ~= "function" then
		if TDVR.campaign.tries > 600 then
			TDVR.note("campaign", "saveAndStartLevel never appeared")
			TDVR.campaign.done = true
		end
		return
	end

	local entry = pickMission()
	if not entry then
		if TDVR.campaign.tries > 600 then
			TDVR.note("campaign", "no mission entry resolved")
			TDVR.campaign.done = true
		end
		return
	end

	SetString("options.tdvrlevel", entry.file or "?")
	if not pcall(saveAndStartLevel,
		TDVR.campaign.mission, entry.title or "", "",
		entry.file, entry.layers, false) then
		TDVR.note("campaign", "StartLevel threw")
		-- Leave done=false so the next tick retries rather than giving up on a
		-- transient failure.
		return
	end

	TDVR.campaign.done = true
	SetString("options.tdvrstage", "level-start")
	TDVR.heartbeat()
end
