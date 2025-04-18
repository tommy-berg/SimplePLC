cmake_minimum_required(VERSION 3.18)
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add strict compiler flags for better error detection
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -pedantic")
    
    # Add additional warnings that help catch common errors
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wshadow -Wconversion -Wunreachable-code")
    
    # Specifically for macOS/Linux compatibility issues
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wundef -Wold-style-cast")
endif()

# ───────────── Lua (works on all platforms) ─────────────
find_package(Lua REQUIRED)  # built-in module

add_library(lua::lua INTERFACE IMPORTED)
set_target_properties(lua::lua PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES      "${LUA_LIBRARIES}"
)

# ────────────── libmodbus (platform-aware) ──────────────
if(WIN32)
    # vcpkg config
    find_package(libmodbus CONFIG REQUIRED)
    if(TARGET modbus AND NOT TARGET libmodbus::libmodbus)
        add_library(libmodbus::libmodbus ALIAS modbus)
    endif()
else()
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

# ────────────── open62541 (OPC UA) ──────────────
if(WIN32)
    find_package(open62541 CONFIG REQUIRED)
else()
    # Prefer CMake CONFIG if available (e.g. on macOS via Homebrew)
    find_package(open62541 CONFIG QUIET)

    if(NOT TARGET open62541::open62541)
        # fallback to manual import if needed
        find_path(OPEN62541_INCLUDE_DIR open62541/server.h)
        find_library(OPEN62541_LIBRARY NAMES open62541)

        if(NOT OPEN62541_INCLUDE_DIR OR NOT OPEN62541_LIBRARY)
            message(FATAL_ERROR "open62541 not found. Please install it via Homebrew, APT, or manually.")
        endif()

        add_library(open62541::open62541 INTERFACE IMPORTED)
        set_target_properties(open62541::open62541 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${OPEN62541_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES      "${OPEN62541_LIBRARY}"
        )
    endif()
endif()

# Define control macro for atomic functions - this ensures we don't define 
# them when using Open62541 which provides its own implementations
# SIMPLEPLC_DEFINE_ATOMIC_FUNCTIONS is now explicitly NOT defined
# Add any other OPC UA-specific compiler definitions here

# ───────────────── Main Executable ─────────────────
add_executable(SimplePLC
    src/main.cpp
    src/server.cpp
    src/modbus_handler.cpp
    src/lua_hooks.cpp
    src/device_config.cpp
    src/plc_logic.cpp
    src/opcua_server.cpp
)

# ───────────────── Test Client ─────────────────
add_executable(test_client
    src/test_client.cpp
)

# On Unix-like systems, add library search paths from pkg-config
if(NOT WIN32)
    target_link_directories(SimplePLC PRIVATE
        ${LIBMODBUS_LIBRARY_DIRS}
    )
    target_link_directories(test_client PRIVATE
        ${LIBMODBUS_LIBRARY_DIRS}
    )
endif()

target_link_libraries(SimplePLC PRIVATE
    libmodbus::libmodbus
    lua::lua
    open62541::open62541
)

# Add a definition to explicitly tell platform.h that we're using Open62541's atomic functions
target_compile_definitions(SimplePLC PRIVATE
    UA_ATOMIC_OPERATIONS_DEFINED=1
)

target_link_libraries(test_client PRIVATE
    open62541::open62541
)

# Copy script files to build directory
file(COPY ${CMAKE_SOURCE_DIR}/scripts/ DESTINATION ${CMAKE_BINARY_DIR}/)