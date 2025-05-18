# SimplePLC

[![CMake](https://github.com/tommy-berg/SimplePLC/actions/workflows/cmake-multi-platform.yml/badge.svg?branch=main)](https://github.com/tommy-berg/SimplePLC/actions/workflows/cmake-multi-platform.yml)

**SimplePLC** is a lightweight, software-based PLC designed for **educational use**, simulations, and small-scale automation projects.

---

## Features

* Modbus TCP & OPC UA servers
* Lua scripting for control logic
* 100ms cyclic execution with hot-reload
* Cross-platform (Linux, macOS, Windows)

---

## Getting Started

### Build

```bash
mkdir build && cd build
cmake ..
make
```

### Dependencies

**Linux**

```bash
sudo apt install libmodbus lua open62541
```

**macOS**

```bash
brew install libmodbus lua open62541
```

### Run

Ensure the following files are in the same folder (examples in scripts folder):

* `settings.ini`
* `active.plc`
  *(optional)* `world.plc`

Then run:

```bash
./SimplePLC
```

---

## Download

Build it yourself or get pre-built binaries from [GitHub Releases](https://github.com/tommy-berg/SimplePLC/releases).

---

## Credits
* [open62541.org](https://www.open62541.org)
* [libmodbus.org](https://libmodbus.org)

