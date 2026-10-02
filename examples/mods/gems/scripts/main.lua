-- Gems and Lamps: a data mod (four blocks) plus one command that drives a block property from Lua.
--
-- Usage in the console:   lamp 10 70 10 on      lamp 10 70 10 off      lamp 10 70 10 toggle
-- The lamp block is placed with setblock first, for example:   setblock 10 70 10 gems:lamp

local LAMP_ON = "gems:lamp[lit=on]"
local LAMP_OFF = "gems:lamp[lit=off]"

-- Reads the state at a position and reports whether it is a lamp and whether it is lit.
local function lamp_state(x, y, z)
    local state = dfe.get_state(x, y, z)
    if state == nil then return nil end        -- the chunk is not loaded
    local text = dfe.state_name(state)         -- for example "gems:lamp[lit=on]"
    if string.sub(text, 1, 9) ~= "gems:lamp" then return "other" end
    return string.find(text, "lit=on", 1, true) and "on" or "off"
end

dfe.command("lamp", "lamp x y z [on|off|toggle]", function(args)
    local x, y, z, mode = string.match(args, "^(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s*(%a*)$")
    if not x then
        dfe.console("usage: lamp <x> <y> <z> [on|off|toggle]")
        return
    end
    x, y, z = tonumber(x), tonumber(y), tonumber(z)
    local current = lamp_state(x, y, z)
    if current == nil then dfe.console("that position is not loaded"); return end
    if current == "other" then dfe.console("there is no gems:lamp at that position"); return end
    if mode == "" or mode == "toggle" then mode = (current == "on") and "off" or "on" end
    if mode ~= "on" and mode ~= "off" then dfe.console("mode must be on, off or toggle"); return end
    dfe.set_block(x, y, z, mode == "on" and LAMP_ON or LAMP_OFF)
    dfe.console("lamp at " .. x .. " " .. y .. " " .. z .. " is now " .. mode)
end)

dfe.on("world_load", function()
    dfe.log("info", "gems loaded: ruby_block, sapphire_block, amethyst_cluster, lamp")
end)
