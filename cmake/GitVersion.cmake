# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Centlake Software AB
#
# Writes the version header, stamping the project version and the commit the
# build actually came from.
#
# Run as a script (cmake -P) from a build-time target rather than at configure
# time, because a configure-time stamp records whichever commit happened to be
# checked out when cmake last ran. On this project that is not a nicety: the
# artifact of a run is a trace, and a trace that names the wrong build sends
# you looking for a bug in source that never went into the binary.
#
# The header is only rewritten when its contents change, so a rebuild on the
# same commit does not recompile everything that includes it.

execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY "${SRC_DIR}"
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_result)

if(NOT git_result EQUAL 0 OR GIT_DESCRIBE STREQUAL "")
    # A source archive has no repository, and a machine may have no git.
    # Say so rather than invent a hash that would be believed.
    set(GIT_DESCRIBE "no-git")
endif()

configure_file("${IN_FILE}" "${OUT_FILE}.tmp" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                        "${OUT_FILE}.tmp" "${OUT_FILE}")
file(REMOVE "${OUT_FILE}.tmp")
