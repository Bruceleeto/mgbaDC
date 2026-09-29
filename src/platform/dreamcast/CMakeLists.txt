add_executable(mgba-dreamcast main.c)
include(kallistios)
kos_add_romdisk(mgba-dreamcast "${CMAKE_SOURCE_DIR}/assets/dc")
target_compile_definitions(mgba-dreamcast PRIVATE ${FEATURE_DEFINES} ${FUNCTION_DEFINES} ${OS_DEFINES})
target_link_libraries(mgba-dreamcast ${BINARY_NAME})
set_target_properties(mgba-dreamcast PROPERTIES
	OUTPUT_NAME ${BINARY_NAME}
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
