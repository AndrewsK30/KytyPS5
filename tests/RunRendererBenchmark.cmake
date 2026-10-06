if(NOT DEFINED BENCHMARK_EXE OR NOT DEFINED OUTPUT_DIR)
  message(FATAL_ERROR "BENCHMARK_EXE and OUTPUT_DIR are required")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
execute_process(
  COMMAND "${BENCHMARK_EXE}" --renderer-benchmark "${OUTPUT_DIR}" 3 64
  RESULT_VARIABLE result
  TIMEOUT 45
)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "Renderer benchmark failed: ${result}")
endif()
file(STRINGS "${OUTPUT_DIR}/frames.csv" rows)
list(LENGTH rows count)
if(NOT count EQUAL 10)
  message(FATAL_ERROR "Expected header and 9 measured frames, got ${count}")
endif()
file(READ "${OUTPUT_DIR}/workload.json" metadata)
string(JSON version GET "${metadata}" workload_version)
if(NOT version STREQUAL "cube-grid-v2")
  message(FATAL_ERROR "Unexpected workload version: ${version}")
endif()
