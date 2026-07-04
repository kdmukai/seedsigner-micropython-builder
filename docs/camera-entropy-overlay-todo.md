# TODO — Image-entropy LVGL overlay (create in lvgl-screens, integrate in camera_entropy)

**Status:** overlay DONE (Design A: ring shutter + camera icon), **builder wiring DONE +
firmware-build-validated** (2026-07-03, P4-LCD43, clean). Remaining: the SeedSigner runner
must call `camera_entropy.set_labels(_("Capturing image..."), _("Accept"))` before `start()`,
then on-device verify. Cross-repo: `seedsigner-lvgl-screens` (overlay — DONE, branch
`feat/camera-entropy-overlay`) + `seedsigner-micropython-builder` (`camera_entropy.cpp` +
`modcamera_entropy.c` — DONE) → `seedsigner` (one runner line — NOT YET, deferred per user).

**Builder wiring (DONE, uncommitted):** `camera_entropy.cpp` builds the overlay on the Option A
black screen (PREVIEW), flips phase off `capture()`→CAPTURING / first `get_result()`→CONFIRM /
`resume()`→PREVIEW, destroys the overlay + `lv_obj_clean`s the screen on stop (pure-black
teardown); events flow through the existing `seedsigner_lvgl_on_button_selected` seam. New
`cam_entropy_set_labels()` C API + `camera_entropy.set_labels()` binding carry the localized
strings (empty until the runner calls it — nothing hardcoded); the shutter's camera icon is
defaulted inside the overlay. Added `seedsigner` to the component CMake REQUIRES. **Interim
device state until the runner change:** shutter + back + phase transitions work; the Accept
button + "Capturing..." text are blank (no strings passed yet).

**Done so far (uncommitted, submodule branch `feat/camera-entropy-overlay`):**
`components/seedsigner/camera_entropy_overlay.{h,cpp}` (two-phase overlay: hardware-mode
PIL-parity text; touch-mode back + concentric-circle shutter + Accept), the
`camera_entropy_overlay_screen` scaffold + registrations (screenshot_gen, runner_core) +
`scenarios.json` + desktop CMake source lists. Compiles clean for the desktop SDL build.
Screenshots (4 scenarios × 4 res) in builder `screenshots/camera-entropy-overlay/` — awaiting
design approval (esp. shutter: bare circle vs "Capture" label).

**Remaining:** wire into `camera_entropy.cpp` (create on the Option A black screen, phase
off capture()/resume(), route events), pass localized strings from the seedsigner runner,
add the ESP-IDF component CMake entry (already added), then on-device verify.

**Scope — repos NOT involved:** the camera-pipeline repos (`esp-camera-pipeline`,
`esp-board-common`) need **no** changes. `cam_pipeline_entropy` already exposes everything the
overlay needs — `frames_chained()` (progress, via the existing `on_frame` hook), `capture()`,
`get_result()`, `resume()` — and, unlike the scanner, entropy has **no coordinator** to touch
(the scanner needed `esp-board-common`'s `scan_coordinator` to drive its status bar from live
decode outcomes; entropy's overlay is just controls + a frame count). The preview↔review phase
is already known to `camera_entropy.cpp` because the host drives it via `capture()` / `resume()`,
so the builder flips the overlay phase off those calls — no pipeline signal required.

## Why (device-confirmed 2026-07-03)
`camera_entropy` renders **no LVGL overlay** (`camera_entropy.h`: "Mirrors camera_scanner,
minus the overlay/coordinator"). The back button previously seen during image entropy was a
**phantom** — the launching view's TopNav bleeding through the old occlude-not-replace
behaviour. The Option A dedicated black screen (see
`docs/camera-pipeline-teardown-screen-reveal.md`) correctly removed it, which exposes that
entropy has **no controls of its own**: no capture, no back.

The QR scanner is unaffected and continues to behave well — it has a real
`camera_preview_overlay` (touch back button) and a working View re-run on back-out.

## Host contract (already implemented — the overlay must satisfy this)
`run_image_entropy_screen` in seedsigner `src/seedsigner/gui/lvgl_screen_runner.py` already
drives an overlay-style event loop, mirroring the scanner, via `_lv.poll_for_result()`:

- **Preview phase:** `button_selected` → `camera_entropy.capture()`; `topnav_back` → cancel
  (returns `None` → back to the launching view).
- **Review phase** (frozen final frame): `button_selected` → accept (returns `(chain, frame)`);
  `topnav_back` → `camera_entropy.resume()` (reshoot → back to preview).

Nothing emits those events today, so entropy is currently inoperable on-device (cancel only
ever "worked" via the phantom; capture never did). The runner docstring already flags this:
"Confirm the exact capture/accept/reshoot events against the native overlay on-device."

## Work

### 1. `seedsigner-lvgl-screens` — create the overlay
- New overlay (or a considered extension of `camera_preview_overlay`) presenting entropy's
  **two phases**:
  - *Preview:* a shutter/capture control (emits `button_selected`) + back (emits `topnav_back`);
    optional live frame-count/progress readout (`camera_entropy.frames_chained()`).
  - *Review:* accept/confirm (emits `button_selected`) + reshoot/back (emits `topnav_back`).
- Same **passive-view contract** as `camera_preview_overlay`: draws onto a parent the host
  hands it, `..._destroy()` frees nothing structural, no screen ownership (the builder owns the
  screen — the Option A black `cam_scr`).
- Touch-mode affordances (P4 is touch); reuse the scanner overlay's geometry approach
  (in-square controls, gutter back that lands in the static landscape margin, never over live
  pixels). Note: unlike the scanner, entropy has **no overlay gutter-blanking today** and the
  black `cam_scr` covers the gutters — keep that intact.
- Emits into the **same** seedsigner result queue that `poll_for_result` drains
  (`button_selected` / `topnav_back`), matching the runner.
- Open as a PR to the upstream fork per the usual flow.

**Design decision to make first:** new entropy-specific overlay vs. extend
`camera_preview_overlay`. The scanner overlay is a passive status-bar + back; entropy needs an
active *shutter* plus a two-state (preview/review) control set, so a distinct overlay is likely
cleaner — but confirm against the scanner code before deciding.

### 2. `seedsigner-micropython-builder` — integrate into `camera_entropy`
- `ports/esp32/camera_entropy/camera_entropy.cpp`:
  - Create the overlay in `cam_entropy_start()` (parented on the Option A black `cam_scr`,
    above the camera image), destroy in `cam_entropy_stop()` — mirror the scanner overlay
    lifecycle (create-after-pipeline-image so it draws above; destroy-before-pipeline).
  - Add a presenter/state hook so the overlay reflects preview vs review phase + progress, and
    routes its touch events into the seedsigner poll queue. A light direct wiring is probably
    enough (entropy has no decode ring, so no full `scan_coordinator` needed).
  - Fold the overlay create/destroy into the existing rollback/leave-active paths added for
    Option A (fail → `cam_rollback_screen`; stop → leave `cam_scr`).
- Update the `camera_entropy.h` "No overlay" note once added.

## Verify (on device)
Preview shows a capture control + back; capture freezes and review shows accept/reshoot;
back cancels to the launching view (which re-renders per the View-layer contract). No phantom,
no reliance on the previous screen.

## Refs
- Template: `ports/esp32/camera_scanner/camera_scanner.cpp` + lvgl-screens
  `components/seedsigner/camera_preview_overlay.{h,cpp}`.
- Host contract: seedsigner `src/seedsigner/gui/lvgl_screen_runner.py` `run_image_entropy_screen`.
- `docs/camera-pipeline-teardown-screen-reveal.md` (Option A; entropy no-overlay note).
- `docs/camera-pipeline-integration-plan.md` (master pipeline plan).
