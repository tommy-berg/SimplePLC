cmake_minimum_required(VERSION 3.18)
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─────────────── Lua (works everywhere) ───────────────
find_package(Lua REQUIRED)                   # built‑in module

add_library(lua::lua INTERFACE IMPORTED)     # single canonical name
set_target_properties(lua::lua PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES      "${LUA_LIBRARIES}"
)

# ────────────── libmodbus (platform split) ─────────────
if(WIN32)
    # vcpkg provides config + target 'modbus'
    find_package(libmodbus CONFIG REQUIRED)  # exports target 'modbus'
    if(TARGET modbus AND NOT TARGET libmodbus::libmodbus)
        add_library(libmodbus::libmodbus ALIAS modbus)
    endif()
else()
    # Linux / macOS → pkg‑config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)

    if(NOT TARGET libmodbus::libmodbus)
        add_library(libmodbus::libmodbus INTERFACE IMPORTED)
        set_target_properties(libmodbus::libmodbus PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${LIBMODBUS_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES      "${LIBMODBUS_LIBRARIES}"
        )
    endif()
endif()

# ────────────────── Executable ──────────────────
add_executable(SimplePLC
    src/main.cpp
    src/server.cpp
    src/modbus_handler.cpp
    src/lua_hooks.cpp
    src/device_config.cpp
    src/plc_logic.cpp
)

# Add pkg‑config’s -L path on non‑Windows hosts
if(NOT WIN32)
    target_link_directories(SimplePLC PRIVATE ${LIBMODBUS_LIBRARY_DIRS})
endif()

target_link_libraries(SimplePLC PRIVATE
    libmodbus::libmodbus
    lua::lua
)
