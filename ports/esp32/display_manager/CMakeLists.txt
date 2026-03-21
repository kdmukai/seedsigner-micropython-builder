set(srcs "display_manager.cpp")
set(include_dirs ".")
set(component_requires "")


list(APPEND component_requires
    "esp_bsp"
    "esp_lvgl_port"
    "lvgl"
    "espressif__esp_io_expander_tca9554"
    "espressif__esp_lcd_axs15231b"
    "seedsigner"
)


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
