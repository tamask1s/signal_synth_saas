if(NOT DEFINED NM_EXECUTABLE OR NOT DEFINED MODULE_FILE)
    message(FATAL_ERROR "NM_EXECUTABLE and MODULE_FILE are required")
endif()

execute_process(
    COMMAND "${NM_EXECUTABLE}" -D "${MODULE_FILE}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR
)

if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "Unable to inspect module symbols: ${NM_ERROR}")
endif()

if(NOT NM_OUTPUT MATCHES "syn_sig_ra_module")
    message(FATAL_ERROR "mod_syn_sig_ra does not export syn_sig_ra_module")
endif()
