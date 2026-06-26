set(srcs "display_manager.cpp")
set(include_dirs ".")
set(component_requires "")

list(APPEND component_requires
    "board_common"
    "esp_lvgl_port"
    "lvgl"
    "seedsigner"
    "esp_hw_support"   # esp_ldo_regulator.h — power the P4 SD card VDD rail
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)

# Display resolution profile: hardware builds target a single display height to
# avoid compiling unused font sets.  The seedsigner component checks for at least
# one SUPPORT_DISPLAY_HEIGHT_* flag at compile time (gui_constants.h).  Setting
# it PUBLIC here propagates to all components that REQUIRE seedsigner.
# SEEDSIGNER_DISPLAY_HEIGHT is passed from build_firmware.sh via CMAKE_ARGS.
if(NOT DEFINED SEEDSIGNER_DISPLAY_HEIGHT)
    set(SEEDSIGNER_DISPLAY_HEIGHT 320)
endif()
target_compile_definitions(__idf_seedsigner PUBLIC SUPPORT_DISPLAY_HEIGHT_${SEEDSIGNER_DISPLAY_HEIGHT})

# Per-font tiny_ttf glyph-cache cap (SEEDSIGNER_TTF_CACHE_SIZE; default 256 in
# gui_constants.h). docs/font-memory-plan.md Task E + docs/knowledge/cjk-glyph-cache-
# pool-overflow.md: at 256 the per-(font,px) cache index overflows the 128 KB LVGL pool
# under realistic CJK and crashes (LV_ASSERT_MALLOC -> watchdog reboot). cap=64 was
# TESTED (2026-06-25) and REJECTED: it fits on bytes but the eviction churn fragments
# the pool (frag 29%, biggest-block collapse) and is unusable alone. The non-fragmenting
# lever is instance-count dedup (screens Task A). Kept parameterized at the stock 256 to
# measure dedup cleanly (large cap = minimal churn); sweep DOWN via
# -DSEEDSIGNER_TTF_CACHE_SIZE=N only as leftover bytes demand — with fewer deduped
# instances a moderate cap churns/fragments far less than 64 did.
# UPDATE 2026-06-25: dedup+256 still overflowed; cap=128 barely helped (~5 KB) because the
# cap controls only 2 of tiny_ttf's 3 caches — the fixed 256-entry kerning cache (now
# disabled via CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0 in the board sdkconfig) was the
# uncontrolled tenant. Back to stock 256 to measure the kerning-cache removal at low
# fragmentation; only sweep DOWN again if leftover bytes still demand it.
if(NOT DEFINED SEEDSIGNER_TTF_CACHE_SIZE)
    set(SEEDSIGNER_TTF_CACHE_SIZE 256)
endif()
target_compile_definitions(__idf_seedsigner PUBLIC SEEDSIGNER_TTF_CACHE_SIZE=${SEEDSIGNER_TTF_CACHE_SIZE})

# Debug mode: enables dev-only features (boot splash text, etc.).
# Pass -DSEEDSIGNER_DEBUG=OFF for production builds.
option(SEEDSIGNER_DEBUG "Enable debug/dev features" ON)
if(SEEDSIGNER_DEBUG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SEEDSIGNER_DEBUG=0)
endif()
