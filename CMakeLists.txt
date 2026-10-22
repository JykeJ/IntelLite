cmake_minimum_required(VERSION 3.21)

project(IntelLite VERSION 0.1.0 LANGUAGES CXX)

# NG's test suite defaults ON and would otherwise demand Catch2.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

# CommonLibSSE-NG (alandtse/CommonLibVR, branch ng). Clone it into refs/CommonLibVR (see
# BUILD.md), or point INTELLITE_COMMONLIB_DIR at a checkout elsewhere.
set(INTELLITE_COMMONLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/refs/CommonLibVR"
    CACHE PATH "CommonLibSSE-NG checkout (alandtse/CommonLibVR, branch ng)")

# Keep the builder's local paths out of the compiled binary. MSVC embeds source
# file paths in exception-handling metadata and debug info; map the two roots to
# neutral virtual paths so release DLLs do not expose the builder's home directory.
if(MSVC)
    add_compile_options("/experimental:deterministic")
    file(TO_NATIVE_PATH "${INTELLITE_COMMONLIB_DIR}" _intellite_commonlib_native)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _intellite_source_native)
    add_compile_options("/pathmap:${_intellite_commonlib_native}=C:\\src\\CommonLibVR")
    add_compile_options("/pathmap:${_intellite_source_native}=C:\\src\\IntelLite")
endif()

add_subdirectory("${INTELLITE_COMMONLIB_DIR}" commonlib)

# A plain source add_subdirectory does NOT define add_commonlibsse_plugin.
if(NOT COMMAND add_commonlibsse_plugin)
    include("${INTELLITE_COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

# The helper generates the SKSEPlugin_Version + SKSEPlugin_Query export half
# (the plugin declaration). SKSEPlugin_Load is authored by hand in src/plugin.cpp:
# exactly one metadata path.
add_commonlibsse_plugin(IntelLite AUTHOR "jykej" SOURCES
    src/plugin.cpp
    src/skyrimnet_bridge.cpp
    src/panel.cpp
    src/places.cpp
    src/fuzzy.cpp
    src/actors.cpp
    src/npc_index.cpp
    src/tasks.cpp
    src/movement.cpp
    src/hotkey.cpp)

target_compile_features(IntelLite PRIVATE cxx_std_23)
target_compile_options(IntelLite PRIVATE /Zc:preprocessor)
target_precompile_headers(IntelLite PRIVATE src/pch.h)
target_include_directories(IntelLite PRIVATE src)

# nlohmann-json (from vcpkg.json) builds the JSON pushed to the panel page.
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(IntelLite PRIVATE nlohmann_json::nlohmann_json)

# Optional deploy: pass -DINTELLITE_MOD_DIR="<MO2 mod folder root>" at configure time.
#   - the built DLL (and its PDB, when present) into <mod>/SKSE/Plugins;
#   - the repo's Data-shaped `mod/` tree (the Magelight page, etc.) into <mod>/.
# The -P script guards the PDB copy so a missing PDB cannot fail the build, and
# copies mod/ idempotently. Empty INTELLITE_MOD_DIR = no deploy.
set(INTELLITE_MOD_DIR ""
    CACHE PATH "Optional deploy target: an MO2 mod folder root. Empty = no deploy.")
if(INTELLITE_MOD_DIR)
    set(INTELLITE_DEPLOY_DIR "${INTELLITE_MOD_DIR}/SKSE/Plugins")
    set(INTELLITE_MOD_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod")
    add_custom_command(TARGET IntelLite POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DSRC_DLL=$<TARGET_FILE:IntelLite>"
            "-DSRC_PDB=$<TARGET_PDB_FILE:IntelLite>"
            "-DDEST_DIR=${INTELLITE_DEPLOY_DIR}"
            "-DMOD_SRC_DIR=${INTELLITE_MOD_SRC_DIR}"
            "-DMOD_DEST_DIR=${INTELLITE_MOD_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/deploy.cmake"
        COMMENT "Deploying IntelLite.dll + mod content to ${INTELLITE_MOD_DIR}"
        VERBATIM
    )
else()
    message(STATUS "IntelLite: INTELLITE_MOD_DIR not set - deploy skipped (build artifacts stay in the build tree)")
endif()
