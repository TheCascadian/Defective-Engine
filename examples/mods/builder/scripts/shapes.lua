-- Pure geometry helpers with no engine calls, so they can be reused and tested in isolation.
-- Each generator returns a function that yields the next position or nil when the shape is finished.
-- Producing blocks one at a time lets the caller spread a large shape over many game ticks.

local shapes = {}

local function order(a, b)
    if a > b then return b, a end
    return a, b
end

-- A solid box between two corners, inclusive.
function shapes.box(x1, y1, z1, x2, y2, z2)
    x1, x2 = order(x1, x2)
    y1, y2 = order(y1, y2)
    z1, z2 = order(z1, z2)
    local x, y, z = x1, y1, z1 - 1
    return function()
        z = z + 1
        if z > z2 then z = z1; y = y + 1 end
        if y > y2 then y = y1; x = x + 1 end
        if x > x2 then return nil end
        return x, y, z
    end
end

-- A solid sphere around a centre. A block belongs to the sphere when its centre lies within the radius.
function shapes.sphere(cx, cy, cz, radius)
    local inner = shapes.box(cx - radius, cy - radius, cz - radius, cx + radius, cy + radius, cz + radius)
    local limit = radius * radius + radius * 0.5
    return function()
        while true do
            local x, y, z = inner()
            if x == nil then return nil end
            local dx, dy, dz = x - cx, y - cy, z - cz
            if dx * dx + dy * dy + dz * dz <= limit then return x, y, z end
        end
    end
end

-- Number of blocks in a box, used to refuse jobs that are too large before any work starts.
function shapes.box_volume(x1, y1, z1, x2, y2, z2)
    return (math.abs(x2 - x1) + 1) * (math.abs(y2 - y1) + 1) * (math.abs(z2 - z1) + 1)
end

return shapes
