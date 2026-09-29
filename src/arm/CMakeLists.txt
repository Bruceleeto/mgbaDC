include(ExportDirectory)
set(SOURCE_FILES
	arm.c
	decoder-arm.c
	decoder.c
	decoder-thumb.c
	isa-arm.c
	isa-thumb.c)

if(M_ARM_JIT)
	list(APPEND SOURCE_FILES jit/jit.c jit/jit-emit.c)
	if(NOT CMAKE_SYSTEM_NAME MATCHES "^[Dd]ream[Cc]ast$")
		list(APPEND SOURCE_FILES jit/jit-host.c)
	endif()
endif()

set(DEBUGGER_FILES
	debugger/cli-debugger.c
	debugger/debugger.c
	debugger/memory-debugger.c)

source_group("ARM core" FILES ${SOURCE_FILES})
source_group("ARM debugger" FILES ${DEBUGGER_FILES})

export_directory(ARM SOURCE_FILES)
export_directory(ARM_DEBUGGER DEBUGGER_FILES)