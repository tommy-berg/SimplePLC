cmake_minimum_required(VERSION 3.18)
project(SimplePLC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
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
include(FetchContent)
FetchContent_Declare(
    open62541
    GIT_REPOSITORY https://github.com/open62541/open62541.git
    GIT_TAG v1.4.11
)

# Configure open62541 options before making it available
set(UA_ENABLE_AMALGAMATION ON CACHE BOOL "Compile open62541 as single file")
set(UA_ENABLE_SUBSCRIPTIONS ON CACHE BOOL "Enable subscriptions")
set(UA_NAMESPACE_ZERO FULL CACHE STRING "Namespace zero nodeset")

# Make open62541 available
FetchContent_MakeAvailable(open62541)

# Create directories for header files
execute_process(
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/include/open62541
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/include/open62541/plugin
)

# Simpler approach - copy the amalgamated header to both locations
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy 
        ${open62541_BINARY_DIR}/open62541.h
        ${CMAKE_BINARY_DIR}/include/
)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy 
        ${open62541_BINARY_DIR}/open62541.h
        ${CMAKE_BINARY_DIR}/include/open62541/
)

# Create simple pass-through headers that include the local copy
file(WRITE ${CMAKE_BINARY_DIR}/include/open62541/server.h 
    "#include \"open62541.h\"\n")

file(WRITE ${CMAKE_BINARY_DIR}/include/open62541/server_config_default.h 
    "#include \"open62541.h\"\n")

file(WRITE ${CMAKE_BINARY_DIR}/include/open62541/client_config_default.h 
    "#include \"open62541.h\"\n")

file(WRITE ${CMAKE_BINARY_DIR}/include/open62541/client_highlevel.h 
    "#include \"open62541.h\"\n")

file(WRITE ${CMAKE_BINARY_DIR}/include/open62541/plugin/log_stdout.h 
    "#include \"../open62541.h\"\n")

# Also create a target to ensure headers are created at build time
add_custom_target(open62541_headers ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/include/open62541
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/include/open62541/plugin
    COMMAND ${CMAKE_COMMAND} -E copy 
        ${open62541_BINARY_DIR}/open62541.h
        ${CMAKE_BINARY_DIR}/include/
    COMMAND ${CMAKE_COMMAND} -E copy 
        ${open62541_BINARY_DIR}/open62541.h
        ${CMAKE_BINARY_DIR}/include/open62541/
    # Create header wrappers at build time too
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"open62541.h\"" > ${CMAKE_BINARY_DIR}/include/open62541/server.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"open62541.h\"" > ${CMAKE_BINARY_DIR}/include/open62541/server_config_default.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"open62541.h\"" > ${CMAKE_BINARY_DIR}/include/open62541/client_config_default.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"open62541.h\"" > ${CMAKE_BINARY_DIR}/include/open62541/client_highlevel.h
    COMMAND ${CMAKE_COMMAND} -E echo "#include \"../open62541.h\"" > ${CMAKE_BINARY_DIR}/include/open62541/plugin/log_stdout.h
    COMMENT "Copying open62541 headers to match include pattern"
    DEPENDS open62541
)

# Set include paths explicitly for all targets
include_directories(BEFORE ${CMAKE_BINARY_DIR}/include)

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
add_dependencies(SimplePLC open62541 open62541_headers)
target_include_directories(SimplePLC PRIVATE ${CMAKE_BINARY_DIR}/include)

# Disable warnings for open62541 old-style casts
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(SimplePLC PRIVATE "-Wno-old-style-cast")
endif()

# ───────────────── Test Client ─────────────────
add_executable(test_client
    src/test_client.cpp
)
add_dependencies(test_client open62541 open62541_headers)
target_include_directories(test_client PRIVATE ${CMAKE_BINARY_DIR}/include)

# Disable warnings for open62541 old-style casts
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(test_client PRIVATE "-Wno-old-style-cast")
endif()

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
    open62541
)

target_link_libraries(test_client PRIVATE
    open62541
)

# Copy script files to build directory
file(COPY ${CMAKE_SOURCE_DIR}/scripts/ DESTINATION ${CMAKE_BINARY_DIR}/)