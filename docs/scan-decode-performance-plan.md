# QR Scan / Decode Performance — Strategy & Plan

**Status:** IN PROGRESS — Tier 3 instrumentation built + **device-validated on the P4-43**
(2026-07-12, see §0.5); Tiers 1/2/4 pending. Instrumentation uncommitted on three
`feat/scan-decode-instrumentation` branches.
**Date:** 2026-07-12
**Goal:** Improve animated-QR **decode throughput** on the ESP32-P4 boards — decode
enough unique frames/sec to keep up with (or outpace) the Sparrow animation, so a
large payload (e.g. a 200-input 2-of-3 multisig PSBT) scans reliably and quickly.
**Related:**
`ports/esp32/board_common/docs/qr-scanning-performance-requirements.md` (the decode
floor + requirements), `ports/esp32/board_common/docs/knowledge/camera-focus-procedure.md`
(manual focus), `docs/camera-pipeline-debug-hud-brief.md` (the on-screen HUD this
plan feeds).

---

## 0. Current snapshot (rough — NOT a benchmark)

A single brief capture from a live 200-input scan (dense QR), shown for
order-of-magnitude only:

```
cam=24.5fps  disp=12.3fps(skip 0%)  consumer=4.4fps  lock_wait=0.0ms  hold=0.0ms
CAM PPA avg=68ms   DISP INTERVAL avg=81ms   DISP CPU avg=34ms
```

**Treat these as illustrative, not fixed.** This is ~two samples, and the `consumer`
(decode) rate varies *within a single scan* — with focus quality, distance, aim,
stability, and luck. A separate casual observation put it nearer ~5.5 fps. So do **not**
anchor any decision on a specific fps number; the only durable read is the **order of
magnitude — single-digit fps, i.e. ~hundreds of ms per decode** — and that number's
**variance is itself a signal** (see Tier 3).

What *is* structural (not sample-dependent) in that capture:
- **`lock_wait=0 / hold=0`** → the decode task is **not** starved for frames and **not**
  blocked on the frame lock. Whatever the exact rate, the per-frame time is spent in
  **grayscale + quirc compute**, not waiting on I/O.
- **Hypothesis (to confirm with Tier 3, not yet proven):** the loop is **quirc-bound**,
  so the rate should rise with sharper focus / a larger, well-framed QR and fall with
  density, blur, and tilt. The within-scan variance is consistent with this — but two
  confounded samples cannot separate density from focus/aim/distance. Measure before
  asserting.
- The serial stream today is **all display-pipeline noise** (`CAM SKIP`, `CAM PPA`,
  `DISP INTERVAL`, `DISP CPU`, `disp/skip/lock/hold`). The useful **decode** line
  (`decode=..fps gray=..ms quirc=..ms det/s=..`) is behind a *different* Kconfig flag
  that is currently **off**.

**Implication:** the biggest wins are (a) eliminating wasted decode slots (focus →
fewer MISSes) and (b) reducing quirc's per-frame cost (crop size). The C↔MicroPython
boundary and the fountain-decode computation are **not** the bottleneck (see §6).

---

## 0.5 Device-validated findings (2026-07-12)

Tier 3 instrumentation was built, flashed, and validated on the P4-43. The clean decode
stream works (`scan:` + `qr:` lines; display noise gone; no per-frame spam; `drop=0`
throughout). Several hundred windows across multiple hand-held scans of the 200-input
multisig now **replace the two-sample snapshot in §0.** What they show:

**Baseline (QR filling the 480² crop, "close"):**
- **quirc ≈ 190–210 ms/frame** whenever a QR is in frame (gray ≈ 30–45 ms) → ~4 fps
  decode ceiling. Idle (no QR) jumps to ~8 fps (`quirc≈90 ms`). **quirc-bound,
  content-dependent — confirmed with hundreds of samples**, not the earlier two.
- **~40–45% of *located* QRs MISS** (`id=100%` but `ok≈55–60%`): quirc finds the finder
  patterns but fails to decode, each burning a full ~200 ms for zero new info
  (`tot new≈499 miss≈334` over ~223 s). **The single biggest throughput leak.**
- **`rep/s = 0` throughout** → we never decode a frame twice → **no headroom; not
  outpacing the Sparrow animation.** Effective useful rate ≈ **2.2 new/s**.
- **`drop=0` throughout** → the MP consumer never fell behind → **empirically confirms
  §3a** (keep cUR behind the boundary; the one flip condition never fired).

**Distance / pixel-count experiment (camera backed off + refocused, "far"), ~223 s each:**

| Metric | close | far |
|---|---|---|
| quirc (QR in frame) | ~195 ms | **~170 ms** |
| decode fps (QR in frame) | ~3.9 | **~4.3** |
| decode success on *located* QRs | 60% | **55%** |
| `none` (no QR found) | 29 | **139** |
| useful throughput | **2.24 new/s** | 2.15 new/s |

**Lesson (sharpens Tier 4): quirc *is* pixel-count-bound — but you cannot cash that in by
shrinking / backing off the QR.** Fewer QR pixels → faster quirc → higher decode-fps
(exactly as predicted), **but** fewer pixels *per module* drops decode reliability
(60→55% on located QRs) and the smaller target is harder to frame (`none` 29→139), so
**useful throughput stayed flat (~2.2 new/s). decode-fps ≠ throughput.** The real Tier-4
win is quirc speed *without* losing px/module — a **tighter ROI crop** (fewer *background*
pixels around a well-resolved QR), not a smaller QR. (Single hand-held uncontrolled runs;
treat ~2.2 new/s as "same ballpark," not a precise regression.)

**RESOLVED — the misses are a thin decode margin, NOT tearing (static-QR test, 2026-07-12).**
A *static* QR (zero transitions/animation) still missed **~35% of located frames**
(`new=1 rep=344 miss=189 none=32` → decode success among located = 345/534 = **64.6%**).
Tearing needs a transition, so a stationary target would decode ~100% if tearing were the
cause — it didn't. **Tearing ruled out as the dominant cause.** The tell is the *shape*:
`ok%` swings **11→100%** frame-to-frame on a *stationary* target, and the operator found it
hard to hold a stable high-`ok` position. So the decode runs **right at the margin** — nail
focus + distance + stillness and `ok`→89–100% with `det/s`→4.3 (the quirc ceiling), but
sub-mm hand motion / tiny distance-angle changes / motion blur flip it. Root cause =
**focus/resolution + positioning stability** (plausibly + screen moiré), all
position-sensitive.

→ **Lever = widen the decode margin**, not tearing mitigation: (a) more px/module (focus at
the sharp plane + framing), (b) a **stability / working-distance aid** (hand-holding at the
margin is itself a top miss source), (c) bright/short exposure to cut motion blur. **Tier 2
(AF HUD) is elevated** — a live sharpness readout directly fixes "can't find/hold the sweet
spot." Raw speed (Tier 4) is secondary: peak `det/s` already hits the ~4.3 ceiling; the
bottleneck is *reliability*, not speed.

---

## 1. Repos & edit-boundary key

| Repo | Where it sits | Edit rule |
|---|---|---|
| **seedsigner-micropython-builder** (this repo) | `ports/esp32/camera_scanner/`, `bindings/`, board `sdkconfig`/`new_files/`, `docs/` | **Direct edits.** |
| **board_common** (upstream `kdmukai/esp-board-common`) | submodule at `ports/esp32/board_common/` (branch `feat/p4-lcd35-bringup`) — `src/scan_coordinator.c`, `src/board_init.c`, `src/board_pipeline_display_lvgl.c` | **Feature branch in the submodule checkout → user opens PR → re-pin.** |
| **esp-camera-pipeline** (upstream `kdmukai/esp-camera-pipeline`) | nested submodule at `ports/esp32/board_common/components/esp-camera-pipeline/` — `src/esp_cam_pipeline.c`, `components/cam_pipeline_qr/`, `components/k_quirc/` | **Feature branch → user PR → re-pin.** (Nested; commit inner first.) |
| **seedsigner** (Python app) | `~/dev/seedsigner/` — `hardware/scan_consumer.py`, `models/decode_qr.py`, `helpers/ur2/` | **OFF-LIMITS without per-repo authorization.** Shared Python business logic (also runs on Pi Zero CPython). Changes = feature branch there, user opens PR. |
| **seedsigner-lvgl-screens** (LVGL screens) | `~/dev/seedsigner-lvgl-screens/` — `camera_preview_overlay` (the green/gray dot + progress bar rendering) | **OFF-LIMITS.** Feature branch, user PR. |
| **tools/focus-target**, **3d-models/esp32-p4-focus-knob** | `~/dev/tools/…`, `~/dev/3d-models/…` | User's own tool/model repos (manual-focus aids). |

**Design line already established** (from `camera-pipeline-debug-hud-brief.md` §2): the
on-screen debug HUD — FPS, decode stats, **and the AF focus readout** — is a **C-side
sibling layer** built in `camera_scanner.cpp` (+ `board_common`), rendered *outside*
`camera_preview_overlay`. **No change to `seedsigner-lvgl-screens` is required** for the
HUD. The portable overlay stays stats-blind.

---

## 2. The strategy — four tiers, instrumentation first

The tiers are ordered by leverage, but **Tier 3's metric is the instrument for all of
them, so it is built first.** Each tier's value depends on the one above: no point
optimizing decode speed if focus is soft; the live-focus feedback serves the manual
focus; the decode-rate metric tells you whether Tier 4 is even needed and lets you tune
the Sparrow knob.

### Tier 3 (built first) — Decode-rate instrumentation

**What:** a single per-window (~2 s) serial line that answers "are we keeping up":
```
scan: new=3.1/s repeat=0.4/s miss=0.6/s none=0.3/s dropped=0 | consumer=4.4fps gray=..ms quirc=..ms
```
- **new/s** ≈ unique source frames absorbed. `new/s < Sparrow fps` → dropping source
  frames (slow Sparrow, or speed decode). `repeat/s > 0` → outpacing the source
  (headroom to speed Sparrow up). Sweet spot = small nonzero `repeat/s`.
- **dropped** = the coordinator's `dropped_new` (already tracked) — a decoded NEW part
  the MP consumer didn't drain in time. Must be surfaced, never silent.
- **gray/quirc split** — settles whether the per-frame time is pure quirc compute (a real
  wall) or has a scheduling gap to reclaim (if `quirc_ms ≪ the loop period`).
- **variance, not just a mean** — the decode rate swings within a scan (focus, distance,
  aim, stability, luck), so a single averaged fps hides the story. Report min/max (or a
  coarse histogram) per window and cumulative counts over the whole scan, so a slow
  stretch (lost aim, drifted focus) is visible and no decision rests on one number. This
  is also how we'd ever *confirm* the quirc-bound hypothesis (§0) — rate vs measured
  sharpness/density across many frames, not two anecdotes.

**Where / which repo:**
| Piece | File | Repo |
|---|---|---|
| Count NEW/REPEAT/MISS/NONE per window; emit the `scan:` line | `src/scan_coordinator.c` (it is the only layer that sees the classification) | **board_common** |
| Enable `CONFIG_CAM_PIPELINE_QR_DEBUG` (gray/quirc/decode-fps) | Kconfig lives in `components/cam_pipeline_qr/Kconfig`; **enable via board `sdkconfig`** | flag = **esp-camera-pipeline**; enablement = **builder** (board sdkconfig / debug build) |
| Quiet the display noise: gate off `CAM SKIP`, `CAM PPA`, the `disp/skip/lock/hold` summary | `src/esp_cam_pipeline.c` (`CONFIG_CAM_PIPELINE_DEBUG`) | **esp-camera-pipeline** |
| Quiet `DISP INTERVAL`, `DISP CPU` | `src/board_pipeline_display_lvgl.c`, `src/board_init.c` | **board_common** |

### Tier 1 — Decode margin: focus + positioning stability (gates everything)

**Why it is the top lever:** device data (§0.5) shows **~40–45% of *located* QRs MISS**, and
the static-QR test **confirmed the cause is focus/resolution + positioning stability, not
tearing** — even a stationary QR misses ~35%, with `ok%` swinging 11→100% from tiny
focus/distance/motion changes. The decode runs at a **thin margin**; widening it is the
biggest *reliability* multiplier (peak speed is already at the quirc ceiling). Free/manual on
the focus + ergonomics side.

**Where / which repo:**
| Piece | Repo |
|---|---|
| Manual focus tool aid + Siemens-star focus target | **tools/focus-target**, **3d-models/esp32-p4-focus-knob** (user, in progress) |
| **Working-distance / stability aid** (stand, distance guide) — hand-holding at the margin is a top miss source (§0.5) | ergonomics / **tools** (not firmware) |
| Bright/short exposure to cut hand-motion blur (bright-AE already set) | `sdkconfig` AE target — **builder** / esp-camera-pipeline |
| Procedure / calibration doc (already exists; keep current) | `docs/knowledge/camera-focus-procedure.md` — **board_common** |

### Tier 2 — Live focus feedback (ISP AF statistics)

**What:** use the ESP32-P4 ISP's **hardware AF-statistics** unit as a live sharpness
meter, so a user (or the dev calibrating the lens) can dial focus by watching a number
peak. **Data available** (`isp_af_result_t`): `int definition[N]` (sharpness / edge
energy — *higher = sharper*) and `int luminance[N]` (brightness), for **N = 3 windows**,
**1 AF controller** on the P4. It is a **relative hill-climb signal** — it cannot say
"closer/farther," only "getting sharper/softer." Distinct from AF *control* (VCM motor),
which is moot on this fixed-lens OV5647 — we only *read* the statistics.

**Design decisions (from this session):**
- **Window = the center square crop (~90% of it)** — the *same* centered-square ROI the
  decoder already grayscales, so "AF-sharp" ⟺ "quirc-sharp"; ~90% trims frame edges where
  a partially-framed QR would pollute the reading. (v1 = 1 window; the other 2 could later
  add center-vs-corner tilt detection.)
- **Normalize `definition` by `luminance`** so brightness changes don't masquerade as
  focus changes; suppress the readout when luminance says too dark / blown out.

**Where / which repo:**
| Piece | File / area | Repo |
|---|---|---|
| Instantiate `esp_isp_af` controller on the pipeline's ISP processor; window on the center-square ROI; read `definition/luminance` (oneshot or continuous) | ISP/CSI init in the camera pipeline (near `csi_init` / `esp_cam_pipeline.c`) | **esp-camera-pipeline** |
| Surface the AF metric into the C-side HUD (readout in the gutter / overlay_text) | `camera_scanner.cpp` presenter (+ `board_common` `overlay_text`) | **builder** (+ **board_common**) |
| (Optional) expose AF metric to Python | `bindings/modcamera_scanner.c` | **builder** |

> **Open question (risk):** the ISP is managed by `esp_video`; confirm we can obtain the
> `isp_proc_handle_t` to attach a user AF controller (or whether esp_video already owns
> AF-stat config). See §7.

### Tier 4 — Decode speed (partly reclaimable, partly a real wall)

Ordered cheapest-first; **measure the gray/quirc split (Tier 3) before touching anything.**

| Lever | What | File / area | Repo | Notes |
|---|---|---|---|---|
| **Measure split** — ✅ **DONE (§0.5)** | gray ≈ 30–45 ms, quirc ≈ 190–210 ms → **pure quirc compute wall**, no big scheduling gap (`lock_wait=0`). | (Tier 3 flag) | esp-camera-pipeline / builder | Core/prio tuning has little to reclaim. |
| **Tighter ROI crop** (biggest non-HW lever) | quirc cost ∝ pixel count — **confirmed (§0.5)**. But **do NOT shrink/back-off the QR** — that loses px/module → more misses, flat throughput. Win = crop out *background* pixels around a **well-resolved** QR (fewer pixels, same module pitch). | `components/cam_pipeline_qr/` crop config; needs a QR-locator to place the ROI | esp-camera-pipeline (+ board `sdkconfig` in builder) | Coupled to Tier 1 (a sharp QR is what makes a tight ROI safe). |
| **Core / priority** | QR task is pinned **core 1, prio 1** (16 KB PSRAM stack) — deliberately **flattened to 1** (== LVGL/CSI/VM) in the P4 board configs so the heavy quirc decode doesn't preempt the VM's `present()`/overlay updates (the LVGL-lock deadlock fix). **Raising it re-risks that deadlock — not a free lever.** Revisit only if the gray/quirc split shows a reclaimable scheduling gap. | `CONFIG_CAM_PIPELINE_QR_TASK_PRIORITY` (set `=1` in the P4 `sdkconfig.board`) + `cam_pipeline_qr.c` task create | builder (sdkconfig) + esp-camera-pipeline | `lock_wait=0` says it already gets frames. |
| **quirc internals** (hardest, real HW floor) | IRAM placement / icache, fixed-point, RS-decode micro-opt | `components/k_quirc/` | esp-camera-pipeline → **k_quirc** (own upstream; see k_quirc-upstream-sync todo) | Last; least certain payoff. |

---

## 3. Decisions on record

### 3a. cUR stays behind the MicroPython boundary (do **not** pull it into the pipeline for speed)

The proposal "pull cUR fully into the camera pipeline so nothing crosses into
MicroPython" bundles two changes; only one ever mattered for speed, and it's **already
done**:
1. **Native vs interpreted fountain decode** — the real win, already banked. `DecodeQR →
   URDecoder (ur2/decoder.py) → native uUR/cUR (deps/cUR)` runs the XOR + CBOR assembly
   in C.
2. **Eliminating the C↔MP boundary** — what "pull into the pipeline" actually changes,
   and it's negligible: per NEW part, one **~2.5 KB copy** (`K_QUIRC_MAX_PAYLOAD=2560`)
   + a few binding calls, at the observed **single-digit-Hz** NEW-part rate ≈ **well under
   1%** of a hundreds-of-ms/frame budget. This holds even at the *fastest* per-frame time
   we've seen, so it does not depend on the exact (and variable) rate. That the loop is
   quirc-bound rather than boundary-bound rests on `lock_wait=0 / hold=0` (§0), not on any
   fps sample.

**Cost of doing it anyway:** forks the ESP scan path away from the **shared Python**
`DecodeQR`/`ur2`/`scan_consumer` (which also runs on Pi Zero) and from the "one Python
codebase" principle; requires hoisting QR-type detection + multi-format routing into C.

**The one condition that would flip this:** a **nonzero `dropped_new` under load** —
i.e., MP GC stalls long enough (order of a second-plus at these single-digit-Hz rates,
ring depth 8) to overflow the NEW ring
and drop already-decoded parts (full ~200 ms paid, zero credit). **Tier 3 surfaces
`dropped_new` directly** — decide from data. If it's 0, the cheaper fix (if ever needed)
is a deeper ring, not relocating cUR. **Result (§0.5): `drop=0` across every validation
scan → the flip condition never fired. Decision stands.**

### 3b. The progress bar is not the throughput gauge; the green dot is correct

- **Green dot is honest and per-frame.** Green (`PART_COMPLETE`) fires whenever the
  fountain state *changed* — a new fragment recovered **or** a new XOR-mixed frame
  absorbed. Gray (`PART_EXISTING`) only when neither changed. Not flawed, not stale. The
  C `report()` dedups on `(status, percent)`, so consecutive greens at the same integer %
  don't re-fire — the dot *stays* lit rather than re-blinking (the observed behavior).
- **The bar looks frozen because `percent` is integer-quantized** (`int(estimate*100)` in
  `decode_qr.py`). For a large payload, each new mixed frame moves the estimate **well
  under 1%**, so many green frames map to the same integer. The internal float *is*
  advancing; fountain progress is also genuinely non-linear (plateau, then jump on a
  solve cascade).
- **Decision: do not chase a finer on-screen bar.** End-to-end sub-percent resolution
  would touch **4 layers across 3 repos** — `seedsigner` (`get_percent_complete`),
  `builder` (binding + `camera_scanner`), `board_common` (`scan_coordinator` dedup), and
  `seedsigner-lvgl-screens` (overlay render), two of them off-limits — for a cosmetic
  gain. The **real "am I learning fast enough" signal is the `new/s` metric (Tier 3)**,
  which is C-side only (builder + board_common). Leave the bar as a completion indicator.

---

## 4. Sequencing

1. ✅ **Tier 3 instrumentation** (board_common + esp-camera-pipeline + builder sdkconfig)
   — **DONE + device-validated (§0.5).** Uncommitted on three feature branches.
2. ✅ **Take numbers on the 200-input scan** — **DONE (§0.5):** quirc wall confirmed
   (~200 ms), no scheduling gap, `drop=0`, `rep=0` (no headroom), ~40–45% miss on located
   QRs. **Next open measurement: the static-QR test** (focus vs tearing).
3. ✅ **Static-QR test** — **DONE (§0.5): not tearing; it's a thin decode margin**
   (focus/resolution + positioning stability). Sets Tier 1 as the right lever.
4. **Tier 1** decode margin (user-driven) — focus at the sharp plane + a stability/
   working-distance aid + bright exposure. The biggest *reliability* multiplier.
5. **Tier 2** AF-stat HUD (esp-camera-pipeline + builder/board_common HUD) — **elevated to a
   usability fix**: a live sharpness readout so the operator can *find and hold* the sweet
   spot (the exact difficulty seen in §0.5).
6. **Tier 4 tighter ROI crop** (esp-camera-pipeline) — secondary (peak speed already at the
   ~4.3/s ceiling); quirc speed without losing px/module. Re-measure with Tier 3.
7. quirc internals (k_quirc) only if 3–6 leave a real gap.

## 5. Immediate next step

**Static-QR test** — the cheapest next datum, no code: it apportions the ~40–45% miss rate
between focus/resolution and tearing (§0.5), which decides whether Tier 1 or an anti-tearing
approach (slower animation / capture timing) is the right investment. In parallel, **commit
the validated Tier 3 instrumentation** on the three `feat/scan-decode-instrumentation`
branches (pending git authorization).

## 6. What is *not* the bottleneck (so we don't chase it)

- The **C↔MP boundary** (§3a): sub-1% of budget regardless of the exact (variable) rate.
- The **fountain decode compute**: already native (cUR), already fast.
- **Frame acquisition / lock contention**: `lock_wait=0 / hold=0`.
- **On-screen bar resolution** (§3b): a display artifact, not a decode limiter.

## 7. Open questions / risks

- ✅ **Miss cause** — **RESOLVED (§0.5, static-QR test): focus/resolution + positioning
  stability, NOT tearing** (a static QR still misses ~35%). Lever = widen the decode margin
  (px/module + stability aid + bright exposure), not anti-tearing.
- **AF controller vs esp_video:** can we get the `isp_proc_handle_t` to attach
  `esp_isp_new_af_controller`, or does esp_video own ISP-stat config? (Tier 2 blocker to
  resolve first.)
- ✅ **gray/quirc split** — **RESOLVED (§0.5):** ~30–45 ms gray / ~190–210 ms quirc → quirc
  wall, no scheduling gap to reclaim (deprioritizes core/prio tuning).
- ✅ **`dropped_new` under load** — **RESOLVED (§0.5): `drop=0`** → §3a stands.
- **k_quirc upstream** has its own sync obligation (see
  `docs/k_quirc-upstream-sync-and-contrast-stretch-todo.md`) — any k_quirc edit must
  respect that.
