cmake_minimum_required(VERSION 3.16)     
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Lua QUIET)
if (NOT Lua_FOUND)
    find_package(unofficial-lua CONFIG REQUIRED)
    set(LUA_LIBRARIES unofficial-lua::lua)
endif()

if (WIN32)
    find_package(libmodbus CONFIG REQUIRED)   # supplied by vcpkg :contentReference[oaicite:0]{index=0}
    set(LIBMODBUS_TARGET libmodbus::libmodbus)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBMODBUS REQUIRED libmodbus)
endif()

add_executable(SimplePLC
    src/main.cpp
    src/server.cpp
    src/modbus_handler.cpp
    src/lua_hooks.cpp
    src/device_config.cpp
    src/plc_logic.cpp
)

if (WIN32)
    target_link_libraries(SimplePLC PRIVATE ${LIBMODBUS_TARGET} ${LUA_LIBRARIES})
else()
    target_include_directories(SimplePLC PRIVATE
        ${LIBMODBUS_INCLUDE_DIRS} ${LUA_INCLUDE_DIR})
    target_link_directories(SimplePLC PRIVATE ${LIBMODBUS_LIBRARY_DIRS})
    target_link_libraries(SimplePLC PRIVATE ${LIBMODBUS_LIBRARIES} ${LUA_LIBRARIES})
endif()
