set(IDF_TARGET esp32p4)

set(SDKCONFIG_DEFAULTS
    boards/sdkconfig.base
    boards/sdkconfig.p4
    boards/WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35/sdkconfig.board
)

# Insert board hardware sdkconfig from esp-board-common before sdkconfig.board,
# so MicroPython-specific settings in sdkconfig.board take precedence.
if(DEFINED BOARD_CONFIG_DIR AND EXISTS "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
    list(INSERT SDKCONFIG_DEFAULTS 2 "${BOARD_CONFIG_DIR}/sdkconfig.defaults")
endif()
