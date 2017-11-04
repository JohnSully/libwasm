project(wasmrt VERSION 0.1)
file(GLOB GENERIC_SOURCES *.cpp)

include_directories("../lib/")
add_executable(wasmrt ${GENERIC_SOURCES})
target_link_libraries(wasmrt wasm)
