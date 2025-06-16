```lua
-- Simulation Idea: Coffee Machine

-- ================================================
-- ============ CONSTANTS & ENUMS =================
-- ================================================

-- State machine enum
local STATE = {
    IDLE = 0,
    FILL_WATER = 1,
    HEAT_WATER = 2,
    BREW_COFFEE = 3,
    COOL_DOWN = 4,
    COMPLETE = 5,
    EMERGENCY = 99
}

-- Alarm threshold constants
local MAX_TEMP = 95
local MAX_PRESSURE = 200
local AMBIENT_TEMP = 25

-- ================================================
-- ============ SYSTEM VARIABLES ==================
-- ================================================

local water_level = 0
local temperature = AMBIENT_TEMP
local pressure = 50
local brewing_flow = 0
local coffee_strength = 0

local state = STATE.IDLE
local previous_state = STATE.IDLE
local state_ticks = 0
local brew_id = 0
local brew_timer = 0

-- Constants (integer-safe)
local fill_rate = 5
local drain_rate = 5
local heat_rate = 2
local cool_rate = 2
local pressure_rate = 2
local water_capacity = 100

-- First-time setup
local first_cycle = true

-- ================================================
-- ============== UTILITY FUNCTIONS ===============
-- ================================================

function clamp(v, min, max)
    return math.max(min, math.min(max, v))
end

function randomInt(min, max)
    return math.random(min, max)
end

function runTimer(timer)
    return math.max(0, timer - 1)
end

function evaluateBrew(temp, time)
    local optimal = 85
    local score = 100 - math.abs(temp - optimal) * 2 - (300 - time) // 3
    return clamp(score, 0, 100)
end

-- ================================================
-- ============= CAUSE-EFFECT MAPPING =============
-- ================================================

local effects = {
    ["EMERGENCY_STOP"] = function()
        modbus.writeDiscreteInput(3, true)
        state = STATE.EMERGENCY
    end,
    ["OVERTEMP"] = function()
        modbus.writeDiscreteInput(0, true)
        modbus.writeCoil(5, false)  -- Heater off
    end,
    ["OVERPRESSURE"] = function()
        modbus.writeDiscreteInput(1, true)
        modbus.writeCoil(2, true)   -- Open valve to relieve
    end
}

-- ================================================
-- ================= MAIN CYCLE ===================
-- ================================================

function cycle()
    -- ================= First-cycle setpoint init =================
    if first_cycle then
        if modbus.readHoldingRegister(10) == nil then modbus.writeHoldingRegister(10, 85) end
        if modbus.readHoldingRegister(11) == nil then modbus.writeHoldingRegister(11, 70) end
        first_cycle = false
    end

    -- ========== Read setpoints ==========
    local temp_set = modbus.readHoldingRegister(10) or 85
    local strength_set = modbus.readHoldingRegister(11) or 70

    -- ========== Read inputs ==========
    local pump_on = modbus.readCoil(0)
    local heater_on = modbus.readCoil(1)
    local valve_open = modbus.readCoil(2)
    local emergency_stop = modbus.readCoil(3)

    -- ========== Safety logic ==========
    if emergency_stop then effects["EMERGENCY_STOP"]() end
    if temperature > MAX_TEMP then effects["OVERTEMP"]() end
    if pressure > MAX_PRESSURE then effects["OVERPRESSURE"]() end

    -- ========== State management ==========
    if state ~= previous_state then
        state_ticks = 0  -- reset duration counter
        previous_state = state
    else
        state_ticks = state_ticks + 1
    end

    if state == STATE.IDLE then
        if pump_on then state = STATE.FILL_WATER end

    elseif state == STATE.FILL_WATER then
        water_level = clamp(water_level + fill_rate + randomInt(-1, 1), 0, water_capacity)
        if water_level >= water_capacity then
            state = STATE.HEAT_WATER
        end

    elseif state == STATE.HEAT_WATER then
        if temperature < temp_set then
            temperature = temperature + heat_rate
        else
            state = STATE.BREW_COFFEE
            brew_timer = 300
        end

    elseif state == STATE.BREW_COFFEE then
        brew_timer = runTimer(brew_timer)
        temperature = temperature + heat_rate + randomInt(-1, 1)
        if brew_timer == 0 then
            state = STATE.COOL_DOWN
        end

    elseif state == STATE.COOL_DOWN then
        temperature = clamp(temperature - cool_rate, AMBIENT_TEMP, MAX_TEMP)
        if temperature <= AMBIENT_TEMP + 2 then
            state = STATE.COMPLETE
            coffee_strength = evaluateBrew(temperature, 300)
            brew_id = brew_id + 1
        end

    elseif state == STATE.COMPLETE then
        -- Wait for SCADA/operator reset
        if not pump_on then
            state = STATE.IDLE
            water_level = 0
            temperature = AMBIENT_TEMP
            pressure = 0
        end

    elseif state == STATE.EMERGENCY then
        -- Passive cooldown or reset logic
        temperature = clamp(temperature - cool_rate, AMBIENT_TEMP, MAX_TEMP)
        pressure = clamp(pressure - 5, 0, 999)
    end

    -- ========== Pressure dynamics ==========
    pressure = 30 + (temperature * 0.5) + (water_level * 0.2) + randomInt(-2, 2)

    -- ========== Brewing flow control ==========
    if valve_open then
        if brewing_flow < strength_set then
            brewing_flow = brewing_flow + 5
        elseif brewing_flow > strength_set then
            brewing_flow = brewing_flow - 5
        end
    else
        brewing_flow = 0
    end

    -- ========== Clamp and write outputs ==========
    temperature = clamp(temperature, 0, MAX_TEMP)
    water_level = clamp(water_level, 0, water_capacity)
    pressure = clamp(pressure, 0, 999)
    brewing_flow = clamp(brewing_flow, 0, 100)

    -- Integer-safe output to Input Registers (read-only sensors)
    modbus.writeInputRegister(0, math.floor(temperature + 0.5))
    modbus.writeInputRegister(1, math.floor(pressure + 0.5))
    modbus.writeInputRegister(2, math.floor(brewing_flow + 0.5))

    -- Integer-safe output to Holding Registers (read/write status)
    modbus.writeHoldingRegister(0, math.floor(water_level + 0.5))
    modbus.writeHoldingRegister(1, math.floor(brew_timer + 0.5))

    -- Discrete status
    modbus.writeDiscreteInput(2, (state == STATE.EMERGENCY) and 1 or 0)

    -- Integer-safe additional outputs
    modbus.writeInputRegister(3, math.floor(coffee_strength + 0.5))
    modbus.writeInputRegister(4, math.floor(brew_id + 0.5))
    modbus.writeInputRegister(5, state)  -- already integer enum

    -- ========== Log output ==========
    print(string.format(
        "STATE:%d | WATER:%d%% | TEMP:%d°C | PRES:%d kPa | FLOW:%d | BREW:%d | STRENGTH:%d",
        state,
        math.floor(water_level + 0.5),
        math.floor(temperature + 0.5),
        math.floor(pressure + 0.5),
        math.floor(brewing_flow + 0.5),
        math.floor(brew_id + 0.5),
        math.floor(coffee_strength + 0.5)
    ))

end
```