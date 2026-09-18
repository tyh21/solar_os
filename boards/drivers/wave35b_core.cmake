set(SOLAR_OS_BOARD_CORE_DRIVER "wave35b-core")
include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")
list(APPEND SOLAR_OS_BOARD_REQUIRED_PACKAGES board_wave35b_core)
