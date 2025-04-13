cmake_minimum_required(VERSION 3.10)
project(modbus_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBMODBUS REQUIRED libmodbus)

find_package(Lua REQUIRED)
include_directories(${LUA_INCLUDE_DIR})

include_directories(${LIBMODBUS_INCLUDE_DIRS})
link_directories(${LIBMODBUS_LIBRARY_DIRS})

add_executable(modbus_server
    src/main.cpp
    src/server.cpp
    src/modbus_handler.cpp
    src/lua_hooks.cpp
    src/device_config.cpp
    src/plc_logic.cpp
    src/plc_timer.cpp
)
target_link_libraries(modbus_server ${LIBMODBUS_LIBRARIES})
target_link_libraries(modbus_server ${LUA_LIBRARIES})