add_executable(mgba-dreamcast main.c)
include(kallistios)
set(DC_ROMDISK_DIR "${CMAKE_SOURCE_DIR}/assets/dc" CACHE PATH "Directory packed into the romdisk")
kos_add_romdisk(mgba-dreamcast "${DC_ROMDISK_DIR}")
target_compile_definitions(mgba-dreamcast PRIVATE ${FEATURE_DEFINES} ${FUNCTION_DEFINES} ${OS_DEFINES})
target_link_libraries(mgba-dreamcast ${BINARY_NAME})
# Run guest code on the ARM -> SH-4 recompiler instead of the interpreter.
option(DC_JIT "Use the ARM recompiler" ON)
if(DC_JIT AND M_ARM_JIT)
	target_compile_definitions(mgba-dreamcast PRIVATE DC_JIT)
endif()
set_target_properties(mgba-dreamcast PROPERTIES
	OUTPUT_NAME ${BINARY_NAME}
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
