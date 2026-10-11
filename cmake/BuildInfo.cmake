# Commit of this repository, recorded in the library and exposed by the wheel
# (docs/contract.md, section 4): a replayed run must name the code that
# produced it.
#
# Read at configure time, so an incremental build refreshes it on the next
# reconfigure. A build without a .git directory (a Docker context, a source
# archive) passes it explicitly: -DQB_GIT_SHA=<sha>.
set(QB_GIT_SHA "" CACHE STRING "Commit of quant-backtest being built (default: read from git)")

if(QB_GIT_SHA)
  set(QB_BUILD_GIT_SHA "${QB_GIT_SHA}")
else()
  set(QB_BUILD_GIT_SHA "unknown")
  find_package(Git QUIET)
  if(Git_FOUND)
    execute_process(
      COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      OUTPUT_VARIABLE _qb_head
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _qb_git_result
    )
    if(_qb_git_result EQUAL 0)
      set(QB_BUILD_GIT_SHA "${_qb_head}")
      # A tree with uncommitted changes is not the commit it sits on.
      execute_process(
        COMMAND ${GIT_EXECUTABLE} status --porcelain
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE _qb_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(_qb_status)
        string(APPEND QB_BUILD_GIT_SHA "-dirty")
      endif()
    endif()
  endif()
endif()

message(STATUS "quantBacktest build stamp: ${PROJECT_VERSION} ${QB_BUILD_GIT_SHA}")
