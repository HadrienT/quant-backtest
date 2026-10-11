# quantModeling, the derivatives pricing library used by hedging/ and by
# nothing else (blueprint/decisions.md, ADR-003).
#
# GIT_TAG is a commit SHA, never a branch: the same commit of this repository
# always builds against the same pricing. Bumping it is a pull request of its
# own, which reruns the hedging tests (docs/contract.md, section 2).
set(QB_QUANTMODELING_SHA "fdc1a10433c4a24564e563e64b8fc1829c1f2213")

include(FetchContent)

# SYSTEM: its headers are included without this project's warnings.
# quantModeling leaves its tests, CLI, bindings and benchmarks behind options
# that are OFF by default, so only the library target is built.
FetchContent_Declare(quantModeling
  GIT_REPOSITORY https://github.com/HadrienT/quant-modeling.git
  GIT_TAG ${QB_QUANTMODELING_SHA}
  SYSTEM
)
FetchContent_MakeAvailable(quantModeling)

if(NOT TARGET quantModeling)
  message(FATAL_ERROR "quantModeling did not define the target 'quantModeling'")
endif()
