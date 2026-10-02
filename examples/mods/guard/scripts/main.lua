-- Region Guard: protect circular areas from player edits by cancelling block events.
--
--   guard add x z radius      protect every block within radius of (x, z), at any height
--   guard list                show the protected regions
--   guard remove n            remove region number n
--   guard clear               remove every region
--
-- block_place and block_break are cancellable: a handler that returns true stops the edit. They fire for edits
-- made on behalf of the player (the setblock console command today, interaction in the game later). Edits made
-- by mods through dfe.set_block never fire them, so a guard cannot block another mod's script.
-- Regions live in memory only; the API has no mod storage yet, so they reset when the game restarts.

local regions = {}

local function inside(region, x, z)
    local dx, dz = x - region.x, z - region.z
    return dx * dx + dz * dz <= region.r * region.r
end

local function protected(x, z)
    for i, region in ipairs(regions) do
        if inside(region, x, z) then return i end
    end
    return nil
end

local function deny(verb, ev)
    local id = protected(ev.x, ev.z)
    if not id then return false end
    dfe.console("guard: region " .. id .. " does not allow you to " .. verb .. " a block at " .. ev.x .. " " .. ev.y .. " " .. ev.z)
    return true
end

dfe.on("block_break", function(ev) return deny("break", ev) end)
dfe.on("block_place", function(ev) return deny("place", ev) end)

local actions = {}

function actions.add(rest)
    local x, z, r = string.match(rest, "^(%-?%d+)%s+(%-?%d+)%s+(%d+)$")
    if not x then dfe.console("usage: guard add <x> <z> <radius>"); return end
    regions[#regions + 1] = { x = tonumber(x), z = tonumber(z), r = tonumber(r) }
    dfe.console("region " .. #regions .. " protects radius " .. r .. " around " .. x .. " " .. z)
end

function actions.list()
    if #regions == 0 then dfe.console("no regions"); return end
    for i, region in ipairs(regions) do
        dfe.console(i .. ": centre " .. region.x .. " " .. region.z .. ", radius " .. region.r)
    end
end

function actions.remove(rest)
    local n = tonumber(rest)
    if not n or not regions[n] then dfe.console("usage: guard remove <region number from guard list>"); return end
    table.remove(regions, n)
    dfe.console("removed region " .. n)
end

function actions.clear()
    regions = {}
    dfe.console("all regions removed")
end

dfe.command("guard", "guard add|list|remove|clear", function(args)
    local action, rest = string.match(args, "^(%a+)%s*(.*)$")
    local handler = action and actions[action]
    if not handler then dfe.console("usage: guard add <x> <z> <radius> | list | remove <n> | clear"); return end
    handler(rest)
end)
