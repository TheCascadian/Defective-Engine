-- Grass behaviour for the base game. It runs on random ticks, so the cost scales with the number of
-- sampled blocks per second and not with the amount of grass in the world.
local GRASS, DIRT = "base:grass_block", "base:dirt"
local SPREAD_CHANCE = 0.25   -- per random tick of a grass block that has light above it
local DIE_CHANCE = 0.5       -- per random tick of grass that is buried

local function open_above(x, y, z)
    local above = dfe.get_block(x, y + 1, z)
    return above == "dfe:air" or above == "base:tall_grass" or above == "base:flower_red"
        or above == "base:flower_yellow" or above == "base:dead_bush" or above == "base:mushroom"
end

local function buried(x, y, z)
    local above = dfe.get_block(x, y + 1, z)
    return above ~= nil and not open_above(x, y, z) and above ~= "base:water" and above ~= "base:glass" and above ~= "base:leaves"
end

dfe.on("random_tick", function(ev)
    if dfe.block_name(ev.state) ~= GRASS then return end
    if buried(ev.x, ev.y, ev.z) then
        if math.random() < DIE_CHANCE then dfe.set_block(ev.x, ev.y, ev.z, DIRT) end
        return
    end
    if math.random() >= SPREAD_CHANCE then return end
    local tx = ev.x + math.random(-1, 1)
    local tz = ev.z + math.random(-1, 1)
    local ty = ev.y + math.random(-1, 1)
    if dfe.get_block(tx, ty, tz) == DIRT and open_above(tx, ty, tz) then
        dfe.set_block(tx, ty, tz, GRASS)
    end
end)
