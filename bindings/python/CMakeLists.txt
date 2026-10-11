# Use CMake's FindPython, which locates headers from the active interpreter
# (a uv-managed or otherwise non-system one included).
set(PYBIND11_FINDPYTHON ON)

# pybind11 comes from the build environment (pyproject.toml), not from vcpkg:
# vcpkg's port drags in its own Python, which fights the interpreter the wheel
# is built for.
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(quantbacktest module.cpp)

target_link_libraries(quantbacktest PRIVATE quantBacktest)
if(QB_BUILD_HEDGING)
  target_link_libraries(quantbacktest PRIVATE quantBacktest_hedging)
endif()

qb_set_warnings(quantbacktest)

install(TARGETS quantbacktest
  LIBRARY DESTINATION .
  RUNTIME DESTINATION .
)
