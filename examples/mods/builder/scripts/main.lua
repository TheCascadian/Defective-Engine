-- Builder Commands: fill a box or a sphere with a block, spread over many ticks.
--
--   fill x1 y1 z1 x2 y2 z2 block      fill 0 70 0 15 75 15 base:stone
--   sphere x y z radius block         sphere 0 80 0 6 gems:ruby_block
--   builds                            show queued work
--   cancelbuild                       drop all queued work
--
-- A command may use at most 5 million Lua instructions and a tick handler 1 million, so a large shape cannot
-- be placed in one call. The work is queued and a tick handler places BLOCKS_PER_TICK blocks per game tick
-- (20 ticks per second). This pattern is the one to copy for any long running mod task.

local shapes = require("shapes")

local BLOCKS_PER_TICK = 2000
local MAX_VOLUME = 1000000

local queue = {}   -- pending jobs: { next = generator, state = block state, placed = n, skipped = n, label = text }

local function enqueue(label, generator, state)
    queue[#queue + 1] = { label = label, next = generator, state = state, placed = 0, skipped = 0 }
    dfe.console("queued " .. label .. " (" .. #queue .. " job(s) waiting)")
end

-- Reads a block argument. Accepts "base:stone" and "gems:lamp[lit=on]". Returns the state or nil.
local function block_arg(text)
    local state = dfe.block_state(text)
    if state == nil then
        dfe.console("unknown block \"" .. text .. "\". Use a registered name such as base:stone")
    end
    return state
end

dfe.command("fill", "fill x1 y1 z1 x2 y2 z2 block", function(args)
    local a, b, c, d, e, f, name = string.match(args, "^(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s+(%S+)$")
    if not a then dfe.console("usage: fill <x1> <y1> <z1> <x2> <y2> <z2> <block>"); return end
    a, b, c, d, e, f = tonumber(a), tonumber(b), tonumber(c), tonumber(d), tonumber(e), tonumber(f)
    local volume = shapes.box_volume(a, b, c, d, e, f)
    if volume > MAX_VOLUME then
        dfe.console("that box holds " .. volume .. " blocks; the limit is " .. MAX_VOLUME .. ". Use a smaller box")
        return
    end
    local state = block_arg(name)
    if state then enqueue("fill of " .. volume .. " blocks with " .. name, shapes.box(a, b, c, d, e, f), state) end
end)

dfe.command("sphere", "sphere x y z radius block", function(args)
    local x, y, z, r, name = string.match(args, "^(%-?%d+)%s+(%-?%d+)%s+(%-?%d+)%s+(%d+)%s+(%S+)$")
    if not x then dfe.console("usage: sphere <x> <y> <z> <radius> <block>"); return end
    x, y, z, r = tonumber(x), tonumber(y), tonumber(z), tonumber(r)
    if r < 1 or r > 60 then dfe.console("the radius must be between 1 and 60"); return end
    local state = block_arg(name)
    if state then enqueue("sphere of radius " .. r .. " with " .. name, shapes.sphere(x, y, z, r), state) end
end)

dfe.command("builds", "show queued build jobs", function()
    if #queue == 0 then dfe.console("no build jobs queued"); return end
    for i, job in ipairs(queue) do
        dfe.console(i .. ": " .. job.label .. ", placed " .. job.placed .. ", skipped " .. job.skipped)
    end
end)

dfe.command("cancelbuild", "drop all queued build jobs", function()
    local n = #queue
    queue = {}
    dfe.console("dropped " .. n .. " job(s)")
end)

dfe.on("tick", function()
    local job = queue[1]
    if not job then return end
    for _ = 1, BLOCKS_PER_TICK do
        local x, y, z = job.next()
        if x == nil then
            dfe.console("finished " .. job.label .. ": placed " .. job.placed .. ", skipped " .. job.skipped .. " (unloaded)")
            table.remove(queue, 1)
            return
        end
        if dfe.set_block(x, y, z, job.state) then job.placed = job.placed + 1 else job.skipped = job.skipped + 1 end
    end
end)

-- The optional dependency on gems only decides what the load message suggests.
if dfe.block_state("gems:ruby_block") then
    dfe.log("info", "gems is installed, try: sphere 0 90 0 5 gems:ruby_block")
end
