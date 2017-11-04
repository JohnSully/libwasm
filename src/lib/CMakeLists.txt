project(libwasm VERSION 0.1)
file(GLOB GENERIC_SOURCES *.cpp)

IF (WIN32)
	enable_language(ASM_MASM)
	file(GLOB ASM_SOURCES layer/win32/*.asm)
	file(GLOB PLAT_SOURCES layer/win32/*.cpp)
ELSE()
	enable_language(ASM_NASM)
	file(GLOB ASM_SOURCES layer/posix/*.asm)
	set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -f elf64 -g -F dwarf")

	set_property(SOURCE ${ASM_SOURCES} PROPERTY LANGUAGE ASM_NASM)

	file(GLOB PLAT_SOURCES layer/posix/*.cpp)
ENDIF()

add_library(wasm STATIC SHARED ${GENERIC_SOURCES} ${ASM_SOURCES} ${PLAT_SOURCES})
