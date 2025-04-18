
# [Work in Progress] SimplePLC
[![CMake on multiple platforms](https://github.com/tommy-berg/SimplePLC/actions/workflows/cmake-multi-platform.yml/badge.svg?branch=main)](https://github.com/tommy-berg/SimplePLC/actions/workflows/cmake-multi-platform.yml)

A lightweight software-based PLC. Its main use-case is fast-track prototyping and testing for cybersecurity research purposes.

## Description
SimplePLC is a software-based Programmable Logic Controller (PLC) that combines industrial automation protocols with scripting capabilities. It functions as a "soft PLC" - software that emulates traditional PLC functionality on standard computing hardware rather than dedicated industrial hardware.

The system provides a foundation for industrial automation tasks with support for standard industrial protocols, customizable logic, and multi-client capabilities.

Data exchange between the PLC logic and communication protocols is handled through a shared memory mapping that's protected by appropriate thread synchronization mechanisms.

## Current Capabilities
* Modbus TCP Server: Fully implemented with support for multiple concurrent client connections
* OPC UA Server: Provides standardized data exchange for industrial applications
* Protocol Interoperability: Seamless data exchange between Modbus and OPC UA

### Modbus Features
* Complete Register Support
* * Coils (digital outputs)
* * Discrete inputs (digital inputs)
* * Holding registers (analog outputs/setpoints)
* * Input registers (analog inputs)
* Extended Functions: Support for device identification and slave ID reporting
* Connection Management: Tracks client connections with statistics and proper resource management
* Robust Socket Handling

### Programming and Logic
* Lua-scripting to program control logic
* Hot-reload capabilities - update plc code without stopping the system
* 100ms cyclic execution - predictable execution model for control logic

### Architecture
* Multithreaded Design: Separate threads for PLC logic execution and protocol servers
* Cross-Platform: Runs on Linux, macOS, and Windows with static linking for easy distribution

## User Interface & Configuration
* Configuration File: Customize behavior through settings.ini
* Real-Time Statistics: Monitor connections and system performance

## Areas of Improvements
* Proper logging mechanisms
* IEC 61131-3 Compliance (ST/Ladder/FBD)
* Cycle monitoring with detection of scan time violation
* Fault generation for missed deadlines
* Cycle time statisics and monitoring
* Real-time scheduling / better timing guarantees
* Benchmarking and testing on RTOS or patchign linux kernel with PREEMPT_RT
* Multicore for parallel processing
* Robus error handling
* EtherCAT, Profinet
* Proper state machine 
* * Mode transitions
* * Program Flow Control
* Human-Machine-Interface [WIP]

And much much more... 

## Getting Started
Build the project using CMake\
Configure your settings in settings.ini\
Write your control logic in Lua\
Run the PLC with: ```./SimplePLC [optional_config_file]```\
Connect to the PLC using standard Modbus clients (like mbpoll) or OPC UA clients

### Limitations
* Limited error handling for some Modbus function codes
* No real-time guarantees for precise timing (standard OS scheduling)
* Basic logging capabilities
* No support for standard IEC 61131-3 programming languages (only Lua scripting)

### Applications
* Educational environments for learning industrial automation
* Small-scale automation projects
* Protocol conversion between Modbus and OPC UA
* Testing and simulation of industrial control systems
* Development and prototyping of automation solutions


### Dependencies
#### Linux
```sudo apt install libmodbus```\
```sudo apt install lua```\
```sudo apt install open62541```

#### MacOS

```brew install libmodbus```\
```brew install lua```\
```brew install open62541```

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
```active.plc``` - PLC control loop\
```world.plc``` (Optional) - Override registers with simulated or external values

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
![Image](/example/Factory%20IO%20-%20Scene%201/from-a-to-b.png "Conveyor")



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