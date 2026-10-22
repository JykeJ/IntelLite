# Post-build deploy helper (script mode: cmake -D... -P deploy.cmake).
# Copies the built IntelLite.dll and, when present, its PDB into the MO2 mod
# folder, then copies the repo's Data-shaped `mod/` tree to the mod folder root.
# A missing PDB is not an error.

if(NOT DEFINED DEST_DIR OR DEST_DIR STREQUAL "")
    message(FATAL_ERROR "deploy.cmake: DEST_DIR not set")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}")

if(NOT DEFINED SRC_DLL OR SRC_DLL STREQUAL "")
    message(FATAL_ERROR "deploy.cmake: SRC_DLL not set")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC_DLL}" "${DEST_DIR}"
    RESULT_VARIABLE _dll_rc)
if(NOT _dll_rc EQUAL 0)
    message(FATAL_ERROR "deploy.cmake: failed to copy '${SRC_DLL}' to '${DEST_DIR}' (rc=${_dll_rc})")
endif()

# PDB is optional: an empty SRC_PDB or a path that is not on disk is skipped.
if(DEFINED SRC_PDB AND NOT SRC_PDB STREQUAL "" AND EXISTS "${SRC_PDB}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC_PDB}" "${DEST_DIR}"
        RESULT_VARIABLE _pdb_rc)
    if(NOT _pdb_rc EQUAL 0)
        message(WARNING "deploy.cmake: failed to copy '${SRC_PDB}' to '${DEST_DIR}' (rc=${_pdb_rc})")
    endif()
endif()

# The repo's Data-shaped mod content tree (mod/Magelight/... -> <mod>/Magelight/...).
# file(COPY) overwrites changed files and leaves unchanged ones alone, so this is
# idempotent; it does not delete stale files.
if(DEFINED MOD_DEST_DIR AND NOT MOD_DEST_DIR STREQUAL "")
    if(DEFINED MOD_SRC_DIR AND NOT MOD_SRC_DIR STREQUAL "" AND EXISTS "${MOD_SRC_DIR}")
        file(MAKE_DIRECTORY "${MOD_DEST_DIR}")
        file(COPY "${MOD_SRC_DIR}/" DESTINATION "${MOD_DEST_DIR}")
        message(STATUS "deploy.cmake: copied mod content '${MOD_SRC_DIR}' -> '${MOD_DEST_DIR}'")
    else()
        message(WARNING "deploy.cmake: MOD_SRC_DIR '${MOD_SRC_DIR}' not found; skipped mod content copy")
    endif()
endif()
