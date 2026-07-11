# Camera QR-task internal-RAM starvation (fragmentation)

_Root-cause + fix record. Diagnosed and fixed on the P4-43 (WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43),
2026-07-07, with on-device confirmation both of the crash and of the fix._

## Symptom

After a real signing session (scan PSBT + seed QR, native crypto modules loaded) plus repeated visits
to the **PSBT overview** screen and a language switch, opening the camera failed — repeatably:

```
E cam_pipeline_qr: Failed to create QR decode task
E scan_coordinator: QR consumer creation failed
ERROR:...lvgl_screen_runner: camera_scanner.start() failed: OSError('scan_coordinator create failed')
```

It was **hard to reproduce** because it needs a specific amount of internal-heap fragmentation to build
up; a fresh-boot "open/close camera + switch language" loop never triggered it.

## Root cause: internal-heap fragmentation, not exhaustion

`xTaskCreatePinnedToCore()` for the QR decode task returns `!= pdPASS` when it can't get its stack from
**internal DRAM**. A standard FreeRTOS task stack is **internal-only, no PSRAM fallback**, and the QR
stack was **32 KB**. At the failure, instrumentation (`heap_caps_get_*`) showed:

```
INTERNAL free=103627 largest=31744, wanted 32768
```

**101 KB free, but the largest contiguous block was only 31 744 bytes — 1 KB short of the 32 KB the stack
needs.** That is fragmentation (plenty free, no big-enough hole), not exhaustion. The ESP-IDF/LVGL
internal heap is **TLSF: it coalesces adjacent frees but cannot compact**, so any sustained churn of
small internal allocations permanently fragments it, and a large *contiguous* requirement becomes
fragile.

### What churns the internal heap

The dominant source was the **PSBT overview screen** (`seedsigner.cpp`, `psbt_overview_screen`). Its
transaction-flow diagram builds many small `std::vector<lv_point_precise_t>` (input/output curves, the
center bar, per-frame pulse buffers, plus build temporaries in `psbt_cubic`/`psbt_snap_curve`/
`psbt_to_precise`). The **default `std::vector` allocator puts these small blocks in internal RAM**;
building and tearing the screen down across repeated visits fragmented that heap. (Locale switches — font
registration — and repeated camera sessions add smaller amounts.) Note: the *pulse animation itself does
not churn* — `psbt_set_pulse` reuses a persisted buffer via `vector::assign`. It's the **per-visit
build/teardown**, not the animation loop, that fragments.

### Why quirc's allocation was the tipping point

Fragmentation first consumes the *small* holes. Once they were gone, the camera's own internal
allocations were forced into the one remaining large hole:

- At camera start the largest hole was ~48 KB.
- `k_quirc_new()` allocates a **~17 KB context struct** via `K_MALLOC_CONTEXT` (was **internal-preferred**),
  carving 48 KB → **31 744**.
- The 32 KB QR-task stack then had nowhere to fit → fail.

(quirc's 64 KB flood-fill scratch was already falling back to PSRAM — 64 KB never fit the ~56 KB largest
internal hole — so it was *not* the internal consumer; the ~17 KB context was.)

## The natural control: entropy still works while QR fails

In the same fragmented state, **image-entropy capture kept working while QR scan failed** — because the
entropy consumer's task stack is only **8 KB** (`CAM_PIPELINE_ENTROPY_TASK_STACK_SIZE`), which fits the
31 KB hole, whereas the QR task's **32 KB** did not. Same heap, outcome decided purely by contiguous-block
size. (The same fragmentation also surfaced as a C++ `bad_alloc` → `abort()` in the seed-word screen path,
via ESP-IDF's exceptions-disabled `__cxa_allocate_exception` stub — a *different failure surface of the
same root*.)

## The fix (three parts, all confirmed on-device)

| # | Change | Where | Effect |
|---|---|---|---|
| Root | Route the overview geometry vectors to PSRAM via a `psram_alloc` / `pvec` (portable `#ifdef ESP_PLATFORM`; degrades to `new`/`delete` off-device) | `seedsigner.cpp` (`seedsigner-lvgl-screens`) | overview stops fragmenting the internal heap |
| #1 | `K_MALLOC_CONTEXT` + `K_MALLOC_SCRATCH` → `k_malloc_large` (PSRAM) | `k_quirc_internal.h` (`k_quirc`) | removes the ~17 KB internal carve at camera start |
| #2 | QR task stack **32 KB → 16 KB** | `cam_pipeline_qr/Kconfig` (`esp-camera-pipeline`) | right-sizes an over-provisioned stack (measured high-water ~8 KB) to fit fragmented holes |

Root + #1 attack the fragmentation *source* and the camera's internal *demand*; #2 right-sizes the one
allocation that failed. Any one of them resolves the observed crash; together they leave a large margin.

## On-device validation (before → after)

Same repro ("many, many" overview re-entries + Spanish + repeated camera opens):

| | Before | After |
|---|---|---|
| `Failed to create QR decode task` | repeatable | **0** |
| Stack requested | `want 32768` | `want 16384` |
| `pre-qr-task largest` floor | **31 744** | **≥ 49 152** (never lower) |
| cam-start → pre-qr-task carve | 49 152 → 31 744 (−17 K) | 49 152 → **49 152** (no carve) |

`largest` held at 48–56 KB throughout, and cam-start == pre-qr-task every open (Fix #1's carve gone). The
internal heap now stays unfragmented enough that even the *old* 32 KB stack would fit — the root cause is
eliminated and the demand is right-sized on top.

## Follow-on: the SPI-display P4 board (LCD 3.5) — moving the task stack to PSRAM (2026-07-10)

On the **P4 LCD 3.5** (`WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35`, ST7796 **SPI** panel) the same
`Failed to create QR decode task` freeze returned — this time in the **"scan animated PSBT → select
seed"** flow, which **re-opens the camera** for a 2nd session (to scan a SeedQR). All three of the 4.3
fixes above were already in effect (k_quirc context/scratch in PSRAM, 16 KB stack), yet it still failed:

```
E cam_pipeline_qr: Failed to create QR decode task (INTERNAL free=47511 largest=13312, wanted 16384 stack)
E scan_coordinator: QR consumer creation failed
```

Session 1 (the PSBT scan) tore down cleanly; ~7 s later session 2 couldn't get 16 KB contiguous internal.
`camera_scanner.start()` raised `OSError`, the app caught it, and the low-memory state then **wedged the
recovery render** (a silent deadlock — UART goes quiet, no panic/reboot).

### Why the SPI board is different: it spends internal RAM the MIPI-DSI board doesn't

The P4's **GPSPI peripheral is served by AHB-GDMA, which cannot reach PSRAM** (only the AXI-GDMA can). So
on an SPI-panel board every DMA buffer on the display/camera path **must be internal DRAM**:

- **LVGL draw buffers** — internal, double-buffered (`board_init.c`, standard-SPI path). ~75 KB, persistent.
- **Camera preview stripe buffer** — internal DMA (`board_pipeline_display_lvgl.c`), up to ~75 KB during a scan.

The **MIPI-DSI 4.3 keeps its LVGL draw buffers in PSRAM** (`buff_spiram=1`) and blits the camera preview
straight to its full framebuffer — **zero internal DMA buffers**. So the 3.5's internal heap is
structurally ~75–150 KB tighter *during a camera session*. On top of that, the app's PSBT parse +
intervening screens fragment internal DRAM (≈150 KB idle → **47 KB free / 13 KB largest** by the 2nd
camera open). Even the right-sized 16 KB stack has no contiguous hole. (All the *config-driven* PSRAM
moves — `K_QUIRC_SCRATCH_IN_PSRAM`, the font cache/bitmap routing, the 128 KB LVGL pool — were already
mirrored onto the 3.5's `sdkconfig.board`; this was the one remaining internal-only allocation.)

### The fix: put the decode-task stack itself in PSRAM

Right-sizing the stack (4.3, Fix #2) was a mitigation; the categorical fix is to remove the
**contiguous-internal** requirement entirely. Both camera worker tasks now allocate their stack from PSRAM
via `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`:

- `cam_pipeline_qr.c` (16 KB) and `cam_pipeline_entropy.c` (8 KB) — the entropy task shares the identical
  latent risk on the SPI board, so it got the same fix.
- **Safe** because each task is pure CPU (quirc / SHA-256 chaining) over PSRAM-resident frame data and
  **never runs with the flash cache disabled** — the only condition under which a PSRAM stack is unsafe.
- Prereqs `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` + `CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION=y`
  (both already set here). This is a board-agnostic change: the MIPI boards keep working (they just never
  hit the internal wall), so it lands in the shared component, not per-board.

### `...WithCaps` lifecycle gotchas (why this touched more than one line)

1. **TCB stays internal.** `xTaskCreatePinnedToCoreWithCaps` allocates only the **stack** from the given
   caps; the TCB is always `pvPortMalloc` (internal). Good — a TCB in PSRAM is touched by the scheduler
   with the cache potentially off and would be unsafe. (~360 B internal per task remains.)
2. **A WithCaps task must NOT self-`vTaskDelete(NULL)`.** The idle task never frees WithCaps memory, so a
   self-delete **leaks the stack + TCB**. The tasks now **park** (`vTaskSuspend(NULL)`) after their
   orderly-shutdown handshake, and `*_destroy()` reclaims them with **`vTaskDeleteWithCaps(handle)`**.
3. **`vTaskDeleteWithCaps()` is synchronous + cross-core-safe.** It suspends the task, spins until it's
   off-core, then deletes and frees — the exact guarantee the earlier hand-rolled self-delete design was
   built to get (that design existed to avoid a cross-core `vTaskDelete` stack leak). The sem-handshake is
   kept only so the task releases its camera frame before deletion; the old 10 ms "let idle reclaim" yield
   is gone.
4. **ESP-IDF stack depth is in bytes** on the RISC-V port (`portSTACK_TYPE == uint8_t`, so
   `usStackDepth * sizeof(StackType_t) == usStackDepth`), so the same `CONFIG_..._STACK_SIZE` value passes
   unchanged to the WithCaps API.

### On-device validation (P4-35, 2026-07-10)

REPL-driven (`camera_scanner` / `camera_entropy` start/stop), 4 cycles each:

| | QR scanner | Entropy |
|---|---|---|
| `start()` success | **4/4** (was `OSError` on the 2nd real session) | **4/4** |
| task running mid-session | `run=True` | 17 frames chained |
| largest INTERNAL block *during* a session | **86 016** (was 13 312 at the failure; the 16 KB stack is no longer carved from it) | 86 016 |
| internal free *after stop*, every cycle | **149 439** (no per-cycle drift) | **123 587** (no per-cycle drift) |

Flat per-cycle free in both flows ⇒ the WithCaps create / `vTaskDeleteWithCaps` free path **leaks
nothing**. The exact `~150 KB → 47 KB / 13 KB` app fragmentation that triggers the freeze is **not
reproducible from the REPL** (MicroPython's GC heap lives in PSRAM, so REPL allocations can't starve
internal DRAM; the prior session's synthetic scanner repro couldn't either), so the final end-to-end
sign-off is the real "scan animated PSBT → select seed" flow on device.

## Key sources

- `deps/seedsigner-lvgl-screens/components/seedsigner/seedsigner.cpp` — `psram_alloc`/`pvec`, `psbt_overview_screen`
- `ports/esp32/board_common/components/esp-camera-pipeline/components/k_quirc/src/k_quirc_internal.h` — `K_MALLOC_CONTEXT`/`K_MALLOC_SCRATCH`
- `ports/esp32/board_common/components/esp-camera-pipeline/components/cam_pipeline_qr/{Kconfig,src/cam_pipeline_qr.c}` — stack size; PSRAM-stack via `xTaskCreatePinnedToCoreWithCaps`; `Failed to create QR decode task` now logs SPIRAM + INTERNAL `free`/`largest`
- `ports/esp32/board_common/components/esp-camera-pipeline/components/cam_pipeline_entropy/src/cam_pipeline_entropy.c` — same PSRAM-stack fix (8 KB task)
- `ports/esp32/board_common/src/board_init.c` (standard-SPI LVGL draw buffers → internal DMA) + `src/board_pipeline_display_lvgl.c` (camera stripe buffer → internal DMA) — why the SPI board's internal heap is tighter than the MIPI board's
- Related: `docs/knowledge/font-architecture-and-memory-budget.md` (why fonts spend PSRAM, not internal), `micropython-gc-heap-lives-in-psram.md`, `esp32-p4-camera-firmware-bringup.md`
