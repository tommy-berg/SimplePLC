cmake_minimum_required(VERSION 3.16)
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ───────────── Lua (system or vcpkg) ─────────────
find_package(Lua QUIET)  # system (FindLua.cmake)

if (NOT Lua_FOUND)
    message(STATUS "System Lua not found, trying vcpkg's lua...")
    find_package(lua CONFIG REQUIRED)  # vcpkg's lua provides this
    set(LUA_TARGET lua::lua)
else()
    message(STATUS "Found system Lua")
    add_library(lua_fallback INTERFACE)
    target_include_directories(lua_fallback INTERFACE ${LUA_INCLUDE_DIR})
    target_link_libraries(lua_fallback INTERFACE ${LUA_LIBRARIES})
    set(LUA_TARGET lua_fallback)
endif()


# ───────────── libmodbus (system or vcpkg) ─────────────
if (WIN32)
    # vcpkg: installs a config package that exports target 'modbus'
    find_package(libmodbus CONFIG REQUIRED)
else()
    # Linux: use pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)
endif()

# Create alias for consistent linking
if (TARGET modbus AND NOT TARGET libmodbus::libmodbus)
    add_library(libmodbus::libmodbus ALIAS modbus)
endif()

# Decide once what we will link against
if (TARGET libmodbus::libmodbus)
    set(LIBMODBUS_LINK libmodbus::libmodbus)
else()
    set(LIBMODBUS_LINK ${LIBMODBUS_LIBRARIES})   # from pkg-config
endif()

# ───────────── Executable ─────────────
add_executable(SimplePLC
    src/main.cpp
    src/server.cpp
    src/modbus_handler.cpp
    src/lua_hooks.cpp
    src/device_config.cpp
    src/plc_logic.cpp
)

# ───────────── Platform-specific config ─────────────
if (WIN32)
    # vcpkg already handles include and link paths
    target_link_libraries(SimplePLC PRIVATE
        ${LIBMODBUS_LINK}
        ${LUA_TARGET}
    )
else()
    # system pkg-config: manually add include/link dirs
    target_include_directories(SimplePLC PRIVATE
        ${LIBMODBUS_INCLUDE_DIRS}
    )
    target_link_directories(SimplePLC PRIVATE
        ${LIBMODBUS_LIBRARY_DIRS}
    )
    target_link_libraries(SimplePLC PRIVATE
        ${LIBMODBUS_LINK}
        ${LUA_TARGET}
    )
endif()
