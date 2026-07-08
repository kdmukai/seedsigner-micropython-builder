# Camera QR-task internal-RAM starvation — diagnosis & remediation plan

Status: **diagnosis settled from static analysis; Phase 1 (on-device confirmation) pending.**
Owner: TBD. Repro board: WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_43 (P4-43, release target).

---

## 1. Symptom

During live P4 testing: camera start/stop worked repeatedly. After changing the UI language
(Spanish, then Farsi), the next camera open failed immediately with an error screen. Serial log:

```
E (230560) cam_pipeline_qr: Failed to create QR decode task
I (230560) cam_pipeline_qr: QR consumer destroyed
E (230560) scan_coordinator: QR consumer creation failed
...
ERROR:seedsigner.gui.lvgl_screen_runner:camera_scanner.start() failed: OSError('scan_coordinator create failed',)
```

The camera pipeline (CSI init, streaming, capture task) came up fine; only the **QR decode task**
failed to spawn, so `scan_coordinator_create` returned `NULL` → `OSError` → error screen.

Two log lines that look alarming but are **not** the bug:
- `DISP INTERVAL: max=23605979 us` — a counter artifact. The camera display pipeline had *just*
  been initialized, so its first interval sample spans all idle wall-clock since the previous camera
  session. Not a live freeze (confirmed against live observation — the failure was immediate).
- `CAM PPA avg=70957 us`, `DISP CPU avg~28–32 ms` — the camera ran only ~2 s before teardown, and
  Farsi UI rendering is genuinely CPU-heavy (~30 ms/frame). Real, but not the failure cause.

---

## 2. Diagnosis

### 2.1 What actually failed
`xTaskCreatePinnedToCore(qr_decode_task, "qr_decode", CONFIG_CAM_PIPELINE_QR_TASK_STACK_SIZE=32768,
...)` returned `!= pdPASS`
(`ports/esp32/board_common/components/esp-camera-pipeline/components/cam_pipeline_qr/src/cam_pipeline_qr.c:421`).
A FreeRTOS task-create fails for exactly one reason: it could not allocate the task **stack + TCB
from internal DRAM**. A standard `xTaskCreate*` stack is **internal-only — no PSRAM fallback**.

### 2.2 The fragmentation signature
Two internal-only task stacks are created during a camera cold start:
- **CSI capture task: 16 KB** (`CSI_TASK_STACK`, `board_pipeline_camera_csi.c:48`) — **succeeded**.
- **QR decode task: 32 KB** — **failed**.

16 KB placed, 32 KB did not → the internal heap's **largest free block was ≥16 KB but < 32 KB**.
That is a **fragmentation** signature (largest-block too small), not necessarily total exhaustion.

### 2.3 Why a camera cold-start is internal-RAM-hungry (and the ordering hazard)
`cam_pipeline_qr_create` allocates, **before** the failing 32 KB stack request, via `k_quirc_new` +
`k_quirc_resize`. quirc's `k_malloc_fast` / `K_MALLOC_SCRATCH` / `K_MALLOC_CONTEXT` **prefer internal
RAM** (SPIRAM only as fallback) — `esp-camera-pipeline/components/k_quirc/src/k_quirc_internal.h:26`.
Notably the flood-fill scratch is `QUIRC_FLOOD_FILL_STACK * 8 = 8192 * 8 = 64 KB`
(`k_quirc.c:85`, `K_MALLOC_SCRATCH`).

Order of large internal demands at camera start:

| Order | Allocation | Caps | Size |
|---|---|---|---|
| 1 | CSI capture task stack | internal-only | 16 KB |
| 2 | quirc context struct | **internal-preferred** | ~few KB |
| 3 | quirc flood-fill scratch | **internal-preferred** | **64 KB** |
| 4 | **QR decode task stack** | **internal-only, no fallback** | **32 KB** ← FAILS |

The hazard: the 64 KB flood-fill scratch (which *can* fall back to PSRAM) is claimed **first** and
competes with the 32 KB QR stack (which **cannot** fall back). Under a tight/fragmented internal
heap, the fallback-capable allocation starves the no-fallback one. quirc's image/region arrays
(`image`, `pixels`, ~230 KB) are already SPIRAM (`K_MALLOC_IMAGE`), so those are not the problem.

### 2.4 Why language switching is the trigger — and what is ruled out
- **Ruled out — font-pack bytes eating internal RAM.** On MicroPython the Farsi TTF + `runs.bin`
  are read *in Python* and handed to the loader as a dict (`mp_pack_provider`,
  `bindings/modseedsigner_bindings.c`), so they live in the **PSRAM-backed GC heap**. Glyph
  bitmaps, A8 masks, and the glyph-cache index are all routed to PSRAM (see
  `docs/knowledge/font-architecture-and-memory-budget.md`).
- **Remaining candidate — general internal-heap pressure/fragmentation** from (a) LVGL-side
  rendering churn on a locale switch (widget rebuilds, BiDi/shaping temporaries) and (b) cumulative
  fragmentation from the many prior camera start/stop cycles (each churns a 64 KB + 32 KB + 16 KB
  internal footprint).
- **Evidence gap this plan closes:** the font-memory work measured the **LVGL static pool** (a
  separate pre-reserved 128 KB array) thoroughly, but *not* the **ESP-IDF general internal heap's
  largest-free-block** — which is the exact number that governs whether a 32 KB task stack can be
  created. Phase 1 measures it.

---

## 3. Worst-case QR corpus (drives the Phase 2 stack measurement)

From `ports/esp32/board_common/docs/qr-scanning-performance-requirements.md §1` (Sparrow-fidelity,
`btc-datagen`):

| Target | Modules | Version | Format | Tier |
|---|---|---|---|---|
| 2-of-3 multisig descriptor (static) | 77 | v15 | static | default |
| UR animated, small | 81 | v16 | UR | default |
| **UR animated, large/many-frame** | **85** | **v17** | UR | **must-scan baseline** |
| BBQr "low" cap | 89 | v18 | BBQr | high-res mode |
| 3-of-5 multisig descriptor (static) | 93 | v19 | static | high-res mode |
| **BBQr max** | **125** | **v27** | BBQr | **high-res mode — worst case** |

**BBQr-max (v27 / 125 modules) is the documented highest-complexity target**, and the decoder's
`QUIRC_MAX_VERSION 27` cap was chosen precisely to clear it. It sits at the edge of P4 decode
resolution (`0.90 × 480 / 125 ≈ 3.46 px/module`, just above the ~3 px/module floor), so it only
decodes in **high-resolution mode**, tightly framed. That makes it the correct — and hardest —
stimulus for the QR-decode stack high-water measurement.

---

## 4. Remediation plan (phased)

### Phase 1 — Confirm the diagnosis (instrument only, no fix)
- Log internal-heap state right before the camera starts, and after each locale switch:
  ```c
  ESP_LOGW(TAG, "INTERNAL free=%u largest=%u",
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ```
  Placement: top of `scan_coordinator_create` (`ports/esp32/board_common/src/scan_coordinator.c`)
  and just before the QR `xTaskCreatePinnedToCore` (`cam_pipeline_qr.c:421`). Optionally also at the
  locale-switch entry point on the C side (display_manager locale load), not the modified binding.
- Repro: boot → open/close camera a few times → switch Spanish → Farsi → open camera.
- **Decision:** `largest` between 16 KB and 32 KB at the failing open ⇒ **fragmentation confirmed**;
  `free` also low ⇒ exhaustion. Either way, this validates §2 and sizes the remaining phases.

### Phase 2 — Measure the QR-decode task stack high-water (prereq for Fix #2)
- Add `uxTaskGetStackHighWaterMark(NULL)` to `log_debug_metrics()` (`cam_pipeline_qr.c:141`), build
  with `CONFIG_CAM_PIPELINE_QR_DEBUG=y`. (No sdkconfig change — `INCLUDE_uxTaskGetStackHighWaterMark`
  is already 1 in this IDF; the value is in **bytes**, `StackType_t == uint8_t`.)
- Drive it with a **full-density BBQr sequence at v27 / 125 modules**, high-resolution mode, ~90%
  fill, partly at edge-of-focus so Reed-Solomon correction actually fires. **Confirm `total_decodes`
  increments** (real v27 decodes, not `QRMISS`) or the measurement under-reports (the deep path is
  decode+RS, not detect). Flood-fill uses an explicit *heap* stack, so image complexity does not
  deepen the call stack — the measurement is stable and bounded by version/RS.
- Record `peak_used = 32768 − HWM`.

### Phase 3 — Fixes (ranked; leverage and perf-safety agree on order)
- **Fix #1 — highest leverage, ~0 perf cost:** route quirc's flood-fill scratch (64 KB) + context to
  PSRAM — `K_MALLOC_SCRATCH` / `K_MALLOC_CONTEXT` (or at least the flood-fill call) from
  `k_malloc_fast` → `k_malloc_large`. Returns ~64 KB+ of internal contention and removes the
  ordering hazard. Repo: **k_quirc** submodule. Perf: negligible — the pixel arrays it operates on
  are already PSRAM, and the LIFO stack/context have strong cache locality.
- **Fix #2 — free, zero perf:** cut `CONFIG_CAM_PIPELINE_QR_TASK_STACK_SIZE` to
  `peak_used + max(4 KB, ~25%)`. Repo: **esp-camera-pipeline** Kconfig (or per-board sdkconfig).
- **Fix #4 — UX robustness:** on `scan_coordinator create failed`, `gc.collect()` + brief yield +
  retry once. Repo: **seedsigner** (Python). Zero steady-state cost.
- **Fix #3 — only if #1+#2 insufficient; carries perf risk:** move task stacks to PSRAM via
  `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`. Needs
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` (not currently set) + the task must never touch flash
  with cache disabled. CSI task is safe; **the QR task carries ~5–25 % decode-compute risk (a PSRAM
  stack is the expensive kind of PSRAM move) → A/B measure before committing.**

### Phase 4 — Validate
- Re-run the Phase 1 repro; confirm the camera starts cleanly after the Farsi switch.
- Confirm decode throughput unchanged via `CONFIG_CAM_PIPELINE_QR_DEBUG` timing metrics (A/B Fix #1,
  and Fix #3 if used).
- Optional: `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` + `vTaskList`/`uxTaskGetSystemState` to dump every
  task's stack high-water for the whole internal-RAM budget picture.

### Phase 5 — Capture knowledge
- Write `docs/knowledge/camera-qr-task-internal-ram-starvation.md`: the fragmentation signature
  (16 KB CSI OK / 32 KB QR fails), the quirc internal-preferred-alloc ordering hazard, and the fix.

---

## 5. Performance impact summary

| Fix | Leverage | Est. perf impact |
|---|---|---|
| #1 flood-fill+context → PSRAM | highest | negligible (~0–3 % decode; arrays already PSRAM, LIFO cache-friendly) |
| #2 shrink QR stack to measured HWM | high | exactly zero (stack size is reserved space, not runtime cost) |
| #3 task stacks → PSRAM | structural | QR task ~5–25 % decode risk; CSI task fine. Measure first. |
| #4 gc+retry on failure | UX | zero steady-state (failure path only) |

Estimates are reasoned from the allocation map + P4 cache behavior, not yet measured;
`CONFIG_CAM_PIPELINE_QR_DEBUG` timing metrics are the ready A/B harness.

---

## 6. Cross-repo / submodule notes

The fix surface spans nested submodules; "one feature branch" is really coordinated branches:

| Change | Lives in | Submodule path in builder |
|---|---|---|
| This plan doc, Phase 5 knowledge doc | **builder** | (repo root `docs/`) |
| Phase 1 `scan_coordinator` heap log | **esp-board-common** | `ports/esp32/board_common` |
| Phase 1 `cam_pipeline_qr` heap log; Fix #2 Kconfig | **esp-camera-pipeline** | `.../esp-camera-pipeline` |
| Fix #1 quirc alloc routing | **k_quirc** | `.../k_quirc` |
| Fix #4 gc+retry | **seedsigner** | (separate app repo) |

Iterate with dirty submodules + `MP_ALLOW_DIRTY=1 make docker-build-all` per the builder's
MicroPython patch workflow; commit each submodule on its own branch, then bump the pointer upward.

---

## 7. Key source references
- `ports/esp32/board_common/components/esp-camera-pipeline/components/cam_pipeline_qr/src/cam_pipeline_qr.c`
  — `cam_pipeline_qr_create` (task create :421), `qr_decode_task` self-delete (:363), `log_debug_metrics` (:141)
- `.../k_quirc/src/k_quirc_internal.h:26` — `k_malloc_fast` (internal-preferred); `.../k_quirc.c:85` — 64 KB flood-fill scratch
- `ports/esp32/board_common/src/scan_coordinator.c:200` — `scan_coordinator_create`
- `ports/esp32/board_common/src/board_pipeline_camera_csi.c:48,268` — `CSI_TASK_STACK`, capture task create
- `ports/esp32/board_common/docs/qr-scanning-performance-requirements.md` — QR target corpus
- `docs/knowledge/font-architecture-and-memory-budget.md` — why fonts spend PSRAM/flash, not internal RAM
