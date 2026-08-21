# KUBridge's legacy stub archive can require one normalization pass before the
# regular create_pbp_file() fixup. Keep that preliminary pass quiet; the PSPDEV
# packaging pass immediately afterwards validates and reports any real error.
execute_process(
	COMMAND "${FIXUP_TOOL}" "${TARGET_FILE}"
	RESULT_VARIABLE result
	OUTPUT_QUIET
	ERROR_QUIET
)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Preliminary PSP import fixup failed: ${result}")
endif()
