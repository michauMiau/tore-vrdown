-- Teardown VR Mod - Lua Script
-- High-level VR features: input handling, UI, locomotion
-- Communicates with injected DLL via registry

function init()
    DebugPrint("Teardown VR Mod loaded")
    SetRegistryBool("teardown_vr/enabled", true)
    SetRegistryInt("teardown_vr/interpupillary", 64) -- mm
end

function tick(dt)
    if not GetRegistryBool("teardown_vr/enabled") then
        return
    end

    -- Read VR input from DLL (written to registry)
    local vr_left_x = GetRegistryFloat("teardown_vr/input/left/x")
    local vr_left_y = GetRegistryFloat("teardown_vr/input/left/y")
    local vr_right_x = GetRegistryFloat("teardown_vr/input/right/x")
    local vr_right_y = GetRegistryFloat("teardown_vr/input/right/y")

    -- Map VR controller input to game actions
    -- Left stick = movement
    if vr_left_y < -0.5 then
        -- Move forward
    elseif vr_left_y > 0.5 then
        -- Move backward
    end

    if vr_left_x < -0.5 then
        -- Strafe left
    elseif vr_left_x > 0.5 then
        -- Strafe right
    end

    -- Right stick = camera look (handled by DLL at low level)

    -- Trigger = interact
    -- Grip = grab
end

function draw(dt)
    -- VR overlay UI
    if GetRegistryBool("teardown_vr/enabled") then
        UiText(10, 10, "VR MODE ACTIVE", UiColor(255, 0, 0, 255))
    end
end
