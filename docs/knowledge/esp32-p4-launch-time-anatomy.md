# ESP32-P4 SeedSigner launch-time anatomy (power-on → OpeningSplash logo-slide)

**Context (2026-07-10):** the P3 launch-speedup pass measured and attributed the full boot
timeline on the P4-43. Metric endpoint = the OpeningSplash logo-slide (the moment
`Controller.start()` begins animating), NOT the fully-rendered Home screen.

## Measurement method (reusable)

- **Wall-clock:** webcam video of the screen at 30 fps; per-frame average luma (`ffprobe
  signalstats YAVG`) locates reset (screen dark-onset), display-init (white flash), the static
  C-boot-logo plateau, and the slide onset (plateau dip) to ±1 frame. Reset via a host-side
  pyserial RTS toggle (no esptool/docker latency). Caveats: the pre-reset screen must be bright
  (the app dims the backlight after idle → an already-dark screen hides the reset transition);
  serial line arrival times are buffered and unreliable — use the video for wall-clock and
  device-clock deltas for intervals.
- **Python phase:** `[boot-ms]` `time.ticks_ms()` prints in `/main.py` (now part of
  `tools/deploy_app.py`'s template): VM-up, controller-import-done, instance-ready. NOTE the
  ticks epoch does not align with the ESP_LOG `(ms)` clock — only use deltas within one clock.

## Where the ~11 s went (baseline, dev profile)

| Phase | Cost | Notes |
|---|---|---|
| ROM + 2nd-stage bootloader | ~0.4 s | INFO bootloader logs |
| PSRAM init + **SPIRAM memtest** | ~0.6 s | full 32 MB pattern test every boot |
| IDF init + board bring-up (display/touch/SD/LVGL) | ~1.6 s | incl. 100 ms deliberate pre-backlight delay |
| MicroPython VM + FS mount + frozen boot | ~0.5 s | |
| **`import seedsigner.controller`** | **~5.8 s** | THE dominant cost — app-side import chain |
| `Controller.get_instance()` | ~0.5 s | settings/SD/locale load |
| `start()` → logo-slide begins | ~0.5–1.0 s | OpeningSplash setup |
| **Total** | **~10.9 s** | |

## What the firmware-side cuts bought

- **P1 network strip:** 10.95 → 10.08 s. Fewer components initializing + a smaller image (the
  P4 XIPs .text/.rodata from PSRAM — copying a smaller image at boot is measurably faster).
- **Release profile** (`PROFILE=release`): 10.08 → 9.60 s. `SPIRAM_MEMTEST=n` (the memtest is
  bring-up insurance, not a per-boot necessity — RAM doesn't degrade per boot), WARN log levels
  (app + bootloader; the camera constraint is a *ceiling* of INFO on the compiled-in max —
  going below is safe, above risks the ISR-context ESP_LOGD interrupt-WDT crash), LVGL asserts
  and debug lookups off. Coredump/WDT/stack-canaries deliberately kept (cheap, field-valuable).

## The elephant: the app import chain (~5.8 s, ~60%)

`import seedsigner.controller` transitively loads most of the app before any pixel moves.
This is app-repo territory (mpy-cross precompiled files already — this is import *execution*,
not compile). Ideas for that future session: lazy/deferred imports on the controller's hot
path, trimming module-level work, splitting the import graph so the splash can start earlier.
The `[boot-ms]` milestones in every future deploy make progress trivially measurable.

## Traps for future measurers

- esptool's `--before default_reset` bootloader session adds seconds of dark screen that look
  like boot time on video — use the plain RTS hard-reset for timing runs.
- The `(ms)` timestamps in ESP_LOG lines and `time.ticks_ms()` have different epochs.
- A boot right after `write_flash` behaves like any other (no first-boot penalty observed),
  but the screen may be mid-dim from the *previous* session when you start recording.
