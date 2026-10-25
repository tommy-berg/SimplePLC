cmake_minimum_required(VERSION 3.24)
project(SimplePLC
    VERSION 2.0.0
    DESCRIPTION "Soft PLC: IEC 61131-3 Structured Text, Modbus TCP, OPC UA, web panel, REST API and MCP"
    HOMEPAGE_URL "https://github.com/tommy-berg/SimplePLC"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SIMPLEPLC_FETCH_OPEN62541 "Build open62541 from source even if an installed copy exists" OFF)
option(SIMPLEPLC_STATIC "Link the executable statically (Linux with musl, Windows)" OFF)
set(SIMPLEPLC_OPEN62541_TAG "v1.5.9" CACHE STRING "open62541 version built when fetching")

if(SIMPLEPLC_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ───────────── Version ─────────────
set(SIMPLEPLC_VERSION "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _tag
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _not_tagged)
    if(_not_tagged)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
                        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _commit
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_commit)
            set(SIMPLEPLC_VERSION "${PROJECT_VERSION}-dev+${_commit}")
        endif()
    endif()
    # Configure again after a commit, checkout or tag, so the version names the commit that was built
    execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _head_ref
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    foreach(_git_path HEAD packed-refs refs/tags ${_head_ref})
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${_git_path}"
                        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _git_file
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_git_file AND NOT IS_ABSOLUTE "${_git_file}")
            set(_git_file "${PROJECT_SOURCE_DIR}/${_git_file}")
        endif()
        if(_git_file AND EXISTS "${_git_file}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_file}")
        endif()
    endforeach()
endif()

# ───────────── open62541 (installed ≥ 1.4, or fetched) ─────────────
if(NOT SIMPLEPLC_FETCH_OPEN62541)
    find_package(open62541 1.4 CONFIG QUIET)
    if(NOT TARGET open62541::open62541)
        find_path(OPEN62541_INCLUDE_DIR open62541/server.h)
        find_library(OPEN62541_LIBRARY NAMES open62541)
        if(OPEN62541_INCLUDE_DIR AND OPEN62541_LIBRARY)
            file(STRINGS "${OPEN62541_INCLUDE_DIR}/open62541/config.h" _ua_ver
                 REGEX "#define UA_OPEN62541_VER_(MAJOR|MINOR) ")
            string(REGEX MATCH "MAJOR ([0-9]+)" _m "${_ua_ver}")
            set(_ua_major "${CMAKE_MATCH_1}")
            string(REGEX MATCH "MINOR ([0-9]+)" _m "${_ua_ver}")
            set(_ua_minor "${CMAKE_MATCH_1}")
            if(_ua_major GREATER 1 OR (_ua_major EQUAL 1 AND _ua_minor GREATER_EQUAL 4))
                add_library(open62541::open62541 INTERFACE IMPORTED GLOBAL)
                set_target_properties(open62541::open62541 PROPERTIES
                    INTERFACE_INCLUDE_DIRECTORIES "${OPEN62541_INCLUDE_DIR}"
                    INTERFACE_LINK_LIBRARIES "${OPEN62541_LIBRARY}")
            endif()
        endif()
    endif()
endif()
if(TARGET open62541::open62541)
    get_target_property(_ua_imported open62541::open62541 IMPORTED)
    if(_ua_imported)
        # Visible to projects that include SimplePLC with add_subdirectory (e.g. its test harness)
        set_target_properties(open62541::open62541 PROPERTIES IMPORTED_GLOBAL TRUE)
    endif()
else()
    message(STATUS "Building open62541 ${SIMPLEPLC_OPEN62541_TAG} from source")
    include(FetchContent)
    set(UA_NAMESPACE_ZERO "REDUCED" CACHE STRING "" FORCE)
    set(UA_ENABLE_ENCRYPTION "OFF" CACHE STRING "" FORCE)
    set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(UA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(UA_ENABLE_DISCOVERY_MULTICAST OFF CACHE BOOL "" FORCE)
    set(UA_FORCE_WERROR OFF CACHE BOOL "" FORCE)
    set(UA_LOGLEVEL 300 CACHE STRING "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # open62541 turns on link-time optimisation whenever the compiler supports it; with Clang on
    # Linux its archive then holds LLVM bitcode that the system linker cannot read
    if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
    FetchContent_Declare(open62541
        GIT_REPOSITORY https://github.com/open62541/open62541.git
        GIT_TAG ${SIMPLEPLC_OPEN62541_TAG}
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""
        EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(open62541)
    if(TARGET open62541 AND NOT TARGET open62541::open62541)
        add_library(open62541::open62541 ALIAS open62541)
    endif()
endif()

# ───────────── Embedded files (web panel, ST standard library, examples, docs) ─────────────
set(_embed_manifest "")
set(_embed_depends "")
macro(simpleplc_embed logical physical)
    string(APPEND _embed_manifest "${logical}=${physical}\n")
    list(APPEND _embed_depends "${physical}")
endmacro()

file(GLOB_RECURSE _web_assets CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/web/assets/*")
foreach(f IN LISTS _web_assets)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/src/web/assets" "${f}")
    simpleplc_embed("web/${rel}" "${f}")
endforeach()
file(GLOB_RECURSE _examples CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/examples/*/plc.json" "${PROJECT_SOURCE_DIR}/examples/*/*.st")
foreach(f IN LISTS _examples)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/examples" "${f}")
    simpleplc_embed("examples/${rel}" "${f}")
endforeach()
simpleplc_embed("st/stdlib.st" "${PROJECT_SOURCE_DIR}/src/st/stdlib.st")
simpleplc_embed("docs/structured-text.md" "${PROJECT_SOURCE_DIR}/docs/structured-text.md")
simpleplc_embed("docs/st-compliance.md" "${PROJECT_SOURCE_DIR}/docs/st-compliance.md")

set(_gen "${PROJECT_BINARY_DIR}/generated")
file(WRITE "${_gen}/embed_manifest.txt.in" "${_embed_manifest}")
configure_file("${_gen}/embed_manifest.txt.in" "${_gen}/embed_manifest.txt" COPYONLY)
add_custom_command(
    OUTPUT "${_gen}/embedded_files.cpp"
    COMMAND "${CMAKE_COMMAND}" -DMANIFEST=${_gen}/embed_manifest.txt
            -DOUTPUT=${_gen}/embedded_files.cpp -P "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake"
    DEPENDS ${_embed_depends} "${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake" "${_gen}/embed_manifest.txt"
    COMMENT "Embedding web panel, ST standard library, examples and docs"
    VERBATIM)

configure_file("${PROJECT_SOURCE_DIR}/src/app/version.h.in" "${_gen}/app/version.h" @ONLY)

# ───────────── musl's libm (vendored C) ─────────────
# The VM's REAL/LREAL SIN, LN, EXPT, ATAN2, ... (src/vm/fmath.h): the same bits on every
# platform, unlike the system libm. No fused multiply-add and no fast-math (C flags as musl's
# own build, plus -ffp-contract=off for Clang, which contracts by default).
file(GLOB SIMPLEPLC_MUSL_MATH_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/third_party/musl-math/*.c")
add_library(simpleplc_musl_math STATIC ${SIMPLEPLC_MUSL_MATH_SOURCES})
set_target_properties(simpleplc_musl_math PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(simpleplc_musl_math PRIVATE /W0 /fp:precise)
else()
    target_compile_options(simpleplc_musl_math PRIVATE -w -ffp-contract=off -fno-builtin -fno-fast-math
        -frounding-math -fno-math-errno -fno-strict-aliasing)
endif()

# ───────────── SimplePLC ─────────────
file(GLOB_RECURSE SIMPLEPLC_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM SIMPLEPLC_SOURCES "${PROJECT_SOURCE_DIR}/src/main.cpp")

add_library(simpleplc_core STATIC ${SIMPLEPLC_SOURCES} "${_gen}/embedded_files.cpp")
target_include_directories(simpleplc_core PUBLIC "${PROJECT_SOURCE_DIR}/src" "${_gen}")
target_include_directories(simpleplc_core SYSTEM PUBLIC
    "${PROJECT_SOURCE_DIR}/third_party/httplib"
    "${PROJECT_SOURCE_DIR}/third_party/json")
target_link_libraries(simpleplc_core PUBLIC open62541::open62541 simpleplc_musl_math)
target_compile_features(simpleplc_core PUBLIC cxx_std_23)
target_compile_definitions(simpleplc_core PUBLIC
    CPPHTTPLIB_THREAD_POOL_COUNT=8
    JSON_USE_IMPLICIT_CONVERSIONS=0)
if(WIN32)
    target_link_libraries(simpleplc_core PUBLIC ws2_32 bcrypt iphlpapi)
    target_compile_definitions(simpleplc_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(simpleplc_core PUBLIC Threads::Threads)
endif()

if(MSVC)
    set(SIMPLEPLC_WARNINGS /W4 /permissive- /utf-8 /EHsc /bigobj /wd4244 /wd4267)
else()
    set(SIMPLEPLC_WARNINGS -Wall -Wextra -Werror -pedantic -Wshadow -Wconversion
        -Wunreachable-code -Wundef -Wold-style-cast)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)")
        # 32-bit ARM (Raspberry Pi OS 32-bit): hundreds of notes that GCC 7/9 changed how some types are
        # passed; everything here is built by one compiler, so they say nothing
        list(APPEND SIMPLEPLC_WARNINGS -Wno-psabi)
    endif()
endif()
target_compile_options(simpleplc_core PRIVATE ${SIMPLEPLC_WARNINGS})
if(NOT MSVC)
    # REAL arithmetic must give the same bits on every platform (no fused multiply-add)
    set_source_files_properties("${PROJECT_SOURCE_DIR}/src/vm/vm.cpp" "${PROJECT_SOURCE_DIR}/src/st/consteval.cpp"
        "${PROJECT_SOURCE_DIR}/src/st/sema.cpp" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()

add_executable(simpleplc src/main.cpp)
target_link_libraries(simpleplc PRIVATE simpleplc_core)
target_compile_options(simpleplc PRIVATE ${SIMPLEPLC_WARNINGS})
if(SIMPLEPLC_STATIC AND NOT MSVC AND NOT APPLE)
    target_link_options(simpleplc PRIVATE -static)
endif()

include(GNUInstallDirs)
install(TARGETS simpleplc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
