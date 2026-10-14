# Writes OUT (a header defining PINOL_BUILD_SHA) from the Git revision of SRC, at every build: an incremental build
# after a commit carries the new revision, "-dirty" marks uncommitted changes. The file is rewritten only when its
# content changes, so nothing is recompiled for nothing.
execute_process(COMMAND git rev-parse --short=12 HEAD WORKING_DIRECTORY "${SRC}"
    OUTPUT_VARIABLE sha ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT sha)
  set(sha "unknown")
else()
  execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY "${SRC}"
      OUTPUT_VARIABLE changes ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(changes)
    set(sha "${sha}-dirty")
  endif()
endif()
set(content "#pragma once\n#define PINOL_BUILD_SHA \"${sha}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL content)
  file(WRITE "${OUT}" "${content}")
endif()
