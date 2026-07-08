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

## Key sources

- `deps/seedsigner-lvgl-screens/components/seedsigner/seedsigner.cpp` — `psram_alloc`/`pvec`, `psbt_overview_screen`
- `ports/esp32/board_common/components/esp-camera-pipeline/components/k_quirc/src/k_quirc_internal.h` — `K_MALLOC_CONTEXT`/`K_MALLOC_SCRATCH`
- `ports/esp32/board_common/components/esp-camera-pipeline/components/cam_pipeline_qr/{Kconfig,src/cam_pipeline_qr.c}` — stack size; `Failed to create QR decode task` now logs `free`/`largest`
- Related: `docs/knowledge/font-architecture-and-memory-budget.md` (why fonts spend PSRAM, not internal), `micropython-gc-heap-lives-in-psram.md`
