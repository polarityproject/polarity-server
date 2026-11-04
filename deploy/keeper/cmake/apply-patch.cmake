# Applies PATCH to the working directory unless it is applied already, so a
# FetchContent PATCH_COMMAND (or an execute_process on every configure) can
# run again.
#   cmake -DPATCH=<file> -P apply-patch.cmake
# Applied patches leave a marker (.sfvr-patched-<name>) in the working
# directory: with several patches on one tree, a later one's lines can sit in
# an earlier one's context, so checking the earlier one in reverse fails once
# both are in. Trees patched before the markers fall back to that check.
get_filename_component(name ${PATCH} NAME)
set(marker ${CMAKE_CURRENT_BINARY_DIR}/.sfvr-patched-${name})
file(SHA256 ${PATCH} hash)
if(EXISTS ${marker})
  file(READ ${marker} applied_hash)
  if(applied_hash STREQUAL hash)
    return()
  endif()
  message(FATAL_ERROR "${name} changed since it was applied here: start from a clean source tree (git checkout -- . in ${CMAKE_CURRENT_BINARY_DIR})")
endif()
execute_process(COMMAND git apply --reverse --check --ignore-whitespace ${PATCH}
                RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
if(NOT applied EQUAL 0)
  execute_process(COMMAND git apply --ignore-whitespace ${PATCH} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "cannot apply ${PATCH}")
  endif()
endif()
file(WRITE ${marker} ${hash})
