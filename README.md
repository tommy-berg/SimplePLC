# Work in progress - SoftPLC Implmentation - Specialization Project

## Usage
Compile and run executable in the same folder as script.lua and settings.ini

## Current state
Not very usefull.
Responds to nmap according to settings.ini
Implemented a basic on/off loop on modbus coil address 1 for debug purposes

## Todo
Implement basic TON timer - :white_check_mark:
Implement TOF timer
Implement TP timer
Implement Lua bindings and hooks
Expand function codes to modbus_handler
Persistent timer state across scans
Integrate LuaHooks into cycle execution