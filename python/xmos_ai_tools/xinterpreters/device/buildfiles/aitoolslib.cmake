set(XMOS_AITOOLSLIB_DEFINITIONS
        "TF_LITE_STATIC_MEMORY"
		"TF_LITE_STRIP_ERROR_STRINGS"
		"XCORE"
		"NO_INTERPRETER"
    )

set(XMOS_AITOOLSLIB_LIBRARIES "${CMAKE_CURRENT_LIST_DIR}/../lib/libxtflitemicro.a")
set(XMOS_AITOOLSLIB_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/../include")
