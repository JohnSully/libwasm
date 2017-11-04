project(wasm_test VERSION 0.1)
file(GLOB GENERIC_SOURCES *.cpp)

include_directories("../lib/")
add_executable(testhost ${GENERIC_SOURCES})
target_link_libraries(testhost wasm)
