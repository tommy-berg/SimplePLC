function cycle()
    local boxAtEnd = modbus.readCoil(0)

    if boxAtEnd then
        modbus.writeDiscreteInput(0, true)
    else
        modbus.writeDiscreteInput(0, false)
    end
end