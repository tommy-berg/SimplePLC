-- This is an example file overriding holding registers
-- TODO: Rewrite to support other types of registers.

function override_register(address)
    if address == 1 then
        return math.random(1, 1000)
    elseif address == 2 then
        return 1234
    end
end

