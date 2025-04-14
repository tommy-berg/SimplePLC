
# [Work in Progress] SimplePLC

A lightweight software-based PLC. Its main use-case is fast-track prototyping and testing for cybersecurity research purposes.

## Description
This project is a software-based PLC (Programmable Logic Controller) implementation that combines Modbus communication capabilities with Lua scripting for control logic. It's a "soft PLC" implementation, meaning it's a software program that emulates PLC functionality on standard computing hardware rather than dedicated PLC hardware.

## Key Components
* I/O Operations via Modbus for Industrial Communication
* Supports read/write coils, descret input, holding register and input registers.
* Includes device identification and slave ID reporting capabilities
* Lua is used to program the PLC
* Event loop architechure

## Areas of Improvements
* Static linking of libraries for easy distribution to Linux/Mac/Windows
* Proper logging mechanisms
* Hot-reload of PLC user-code
* IEC 61131-3 Compliance (ST/Ladder/FBD)
* Real-time scheduling
* Robus error handling
* EtherCAT, Profinet
* MTQQ or OPC/UA support
* Proper state machine 
* * Mode transitions
* * Program Flow Control
* Human-Machine-Interface [WIP]

And much much more... 

## Getting Started


### Dependencies
#### Linux
```sudo apt install libmodbus```\
```sudo apt install lua```

#### MacOS

```brew install libmodbus```\
```brew install lua```

### Installing

```
mkdir build
cd build
cmake ..
make
```

### Configure and run
Program requires two files to start:

```settings.ini``` - Device configuration\
```plc_logic.lua``` - PLC control loop\
```world.lua``` (Optional) - Override registers with simulated or external values

Run ```./SimplePLC```  from the same folder where the above mentioned files exist.

## Example - Emulating modbus devices
Emulate device identification  via ```settings.ini```

```
❯ nmap --script modbus-discover.nse -p 502 127.0.0.1
Starting Nmap 7.95 ( https://nmap.org ) at 2025-04-15 01:19 CEST
Nmap scan report for localhost (127.0.0.1)
Host is up (0.00016s latency).

PORT    STATE SERVICE
502/tcp open  modbus
| modbus-discover:
|   sid 0x1:
|     Slave ID data: \xFA\xFFPM710PowerMeter
|_    Device identification: Schneider Electric PM710 v03.110

Nmap done: 1 IP address (1 host up) scanned in 0.07 seconds
````

## Example - controling a conveyor in Factory IO
The following code-snippet controls a conveyor in FactoryIO. FactoryIO must run on a Windows PC and must be configured to connect as a modbus client to the IP-address of the machine running SimplePLC.


The sample code controls one of the default scenes in FactoryIO ("From A to B" template).
````
function cycle()
    local boxAtEnd = modbus.readCoil(0)

    if boxAtEnd then
        modbus.writeDiscreteInput(0, true)  -- Start conveyor
        print("No box detected, starting conveyor")
    else
        modbus.writeDiscreteInput(0, false) -- Stop conveyor
        print("Box detected, conveyor stopped")
    end
end
````

## Example - Internal simulation
Use ```world.lua```to fetch remote data to modbus addresses or to simulate a industrial application if no other professional simulation platform like Factory IO is available to you.

... 

## Lua help
```plc_logic.lua``` can use the following operations:
```
Read Operations:
- modbus.readCoil(address) -- Read digital actuator values
- modbus.readDiscreteInput(address) -- Read digital sensor values
- modbus.readHoldingRegister(address) -- Read analog actuator values
- modbus.readInputRegister(address) -- Read analog sensor values

Write Operations:
- modbus.writeCoil(address, value) -- Write digital actuator values
- modbus.writeDiscreteInput(address, value) -- Write digital sensor values
- modbus.writeHoldingRegister(address, value) -- Write analog actuator values
- modbus.writeInputRegister(address, value) -- Write analog sensor values
```

## Acknowledgements
Thanks to FactoryIO for providing a trail license for their software.