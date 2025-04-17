cmake_minimum_required(VERSION 3.18)
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ───────────── Lua (system or vcpkg) ─────────────
find_package(Lua QUIET)

if (NOT Lua_FOUND)
    message(STATUS "System Lua not found, trying vcpkg's lua...")
    find_package(lua CONFIG REQUIRED)  # vcpkg's lua provides this
    if (TARGET lua::lua)
        set(LUA_TARGET lua::lua)
    else()
        message(FATAL_ERROR "vcpkg's Lua target 'lua::lua' is missing.")
    endif()
else()
    message(STATUS "Found system Lua")
    add_library(lua_fallback INTERFACE)
    target_include_directories(lua_fallback INTERFACE ${LUA_INCLUDE_DIR})
    target_link_libraries(lua_fallback INTERFACE ${LUA_LIBRARIES})
    set(LUA_TARGET lua_fallback)
endif()

# ───────────── libmodbus (system or vcpkg) ─────────────
if (WIN32)
    find_package(libmodbus CONFIG REQUIRED)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)
endif()

if (TARGET modbus AND NOT TARGET libmodbus::libmodbus)
    add_library(libmodbus::libmodbus ALIAS modbus)
endif()

if (TARGET libmodbus::libmodbus)
    set(LIBMODBUS_LINK libmodbus::libmodbus)
elseif (LIBMODBUS_LIBRARIES)
    set(LIBMODBUS_LINK ${LIBMODBUS_LIBRARIES})  # from pkg-config
else()
    message(FATAL_ERROR "libmodbus target or libraries not found.")
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

if (WIN32)
    target_link_libraries(SimplePLC PRIVATE
        ${LIBMODBUS_LINK}
        ${LUA_TARGET}
    )
else()
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