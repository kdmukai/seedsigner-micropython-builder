# Camera pipeline reveals the previous screen (startup + teardown) — builder-side fix

> **STATUS (2026-07-03): builder-side Option A IMPLEMENTED** in
> `ports/esp32/camera_scanner/camera_scanner.cpp` and
> `ports/esp32/camera_entropy/camera_entropy.cpp` — each camera session now builds on its
> own opaque-black screen (created + loaded on start, pre-camera screen deleted on success,
> black screen left active on stop). COMPILE-VALIDATED (clean full P4-LCD43 build, 2026-07-03);
> NOT yet device-validated. The app-side other half
> (re-run the destination View on cancel/back, per the View-layer contract) is tracked in the
> `seedsigner` repo: `docs/_integration/camera-back-out-view-contract-todo.md`.

## Symptom
The **previously-active screen — usually the main menu — is visible to the user at both ends
of a camera session** because the camera composition is drawn *over* it rather than replacing
it. Two windows leak the old screen:

1. **Startup (TODO — not yet addressed):** between `cam_scanner_start()` / `cam_entropy_start()`
   returning and the **first camera frame actually rendering** (camera warm-up), the old screen
   is fully visible underneath. The user sees the screen that *led into* the live preview linger
   for a beat before the preview appears. The user should never see that hand-off screen — the
   display should go straight to black the instant the pipeline starts.
2. **Teardown:** when the session ends (QR scan completes / is cancelled, or entropy capture
   finishes), the old screen **flashes back into view** for the window between camera teardown
   and the host loading the next screen.
3. **Entropy back-out (TODO — worst case):** pressing *back* out of the image-entropy flow
   **leaves the last entropy frame stuck on the panel** and the menu we return to **never
   re-renders**. Two separate faults stack here (see "Entropy back-out: why the frame lingers,
   and who owns the fix" below): the stale frame is a builder-side occlude-not-replace artifact;
   the missing menu re-render is a **main-app** navigation responsibility.

The user should never see the old screen at either boundary; the display should stay black (or
go straight to the next screen) through both hand-offs.

## Why this is the builder's problem, not `seedsigner-lvgl-screens`
The camera overlay in `seedsigner-lvgl-screens`
(`components/seedsigner/camera_preview_overlay.{h,cpp}`) is a **passive view**. It only draws
widgets onto a `parent` the host hands it, and its `..._destroy()` deliberately frees nothing
in the LVGL tree. Its lifetime is fully bounded by that parent — it has no knowledge of when
the camera pipeline starts or stops, does not own the screen, and cannot control what is shown
after teardown.

The gutter-blanking those overlay widgets do (opaque black rects flanking the preview square)
was added precisely because the composition is laid **over** a stale screen — but that black
only exists *while the overlay widgets exist*. Teardown is outside that lifetime, so blanking
can't cover the reveal. The lifecycle — create parent, start camera, stop camera, hand off to
the next screen — lives entirely **here in the builder**.

## Root cause
Both camera compositions build themselves as **children of the previously-active LVGL screen**
rather than on a dedicated screen of their own:

- `ports/esp32/camera_scanner/camera_scanner.cpp` — `cam_scanner_start()`:
  ```c
  s_screen = lv_screen_active();           // captures the menu (or whatever was up)
  ...                                       // camera image + overlay become its children
  ```
- `ports/esp32/camera_entropy/camera_entropy.cpp` — `cam_entropy_start()`: identical
  `s_screen = lv_screen_active();`, camera image rendered as a child of it.

Because the previous screen is **occluded, never replaced**, it is still the active screen the
whole time. On stop:

- `cam_scanner_stop()` / `cam_entropy_stop()` destroy the coordinator/consumer, the overlay
  handle, and the pipeline (which deletes the camera's LVGL image child).
- With the camera image gone and no new screen loaded yet, LVGL simply shows the still-active
  previous screen underneath → **the flash**.

The gap closes only when the host's next SeedSigner `View` runs and calls `lv_screen_load(...)`
(via the app's `load_screen_and_cleanup_previous` path). Everything between stop and that load
shows the old screen.

## This also affects the camera **entropy** pipeline — and more so
`camera_entropy` has the same `lv_screen_active()` occlude-not-replace structure, so it has the
same teardown reveal. It is actually **worse off**: `camera_entropy` has **no overlay at all**
(see the header: *"No overlay"*), so it never gets the gutter-blanking either. (Device-confirmed
2026-07-03: the back button once seen during entropy was a *phantom* from the launching view
bleeding through — Option A removed it, exposing that entropy has no controls of its own. Adding
a real entropy overlay is tracked in `docs/camera-entropy-overlay-todo.md`.) On a landscape
DSI panel that means the stale previous screen shows in the side gutters **for the entire
capture**, not just during teardown. Any fix here should cover both `camera_scanner` and
`camera_entropy`; the entropy path additionally wants the gutters covered during the live
capture, which a dedicated-screen fix (Option A) gives for free.

## Entropy back-out: why the frame lingers, and who owns the fix
When entropy capture is stopped after a still was captured, `cam_pipeline_destroy()` deletes the
LVGL image object — but on a direct-to-panel sink the **last DMA'd frame stays on the panel until
something repaints that region**. Because the camera image was a child of the still-active menu
(occlude-not-replace), deleting it does **not** invalidate the menu underneath, so nothing
overwrites those pixels and the frozen frame lingers on screen.

This splits cleanly into two owners:

- **Builder / screens side (stale frame):** the occlude-not-replace structure is the builder's to
  fix. Option A removes it — the camera lives on its own screen, so the menu is never drawn over
  and there are no orphan camera pixels to leave behind.
- **Main-app side (re-run the destination View):** re-rendering the view we navigate *back* to is
  the **host application's** responsibility, not the screen layer's. This must follow SeedSigner's
  existing **View-layer contract**: a cancel/back button is a decision that **closes the current
  View, forwards to the next destination** (here, the previous View we came from), **and runs that
  View again from the top as if for the first time.** Re-running the destination View — not
  patching or repainting the old screen in place — is what guarantees clean, isolated state at
  every transition. The screens/builder layer must not try to reload or repaint the menu itself.
  The reason it currently doesn't repaint is that occlude-not-replace left the host's screen
  bookkeeping thinking the menu was still loaded, so nothing re-ran the destination View.

  This is not camera-specific — it is the general navigation invariant. The original Python code
  has a handful of violations (generally around complicated worker threads), but the principle
  holds by default and **deviating from it requires a very good reason.**

These two fixes are complementary and reinforce each other: once the builder adopts Option A, the
camera genuinely **replaces** the menu screen, so the host's ordinary navigation bookkeeping is
correct again — backing out is a genuine view reload, which repaints the menu fresh and there are
no stale camera pixels for it to have to paint over. Neither side needs to special-case the other;
each just does its normal job. **Do not** paper over the missing menu re-render inside the screen
layer.

## Fix options (all builder-side)

### Option A — dedicated camera screen (recommended, root-cause)
Give each camera session its **own** black-background screen instead of drawing on the active one.

- On start: `lv_obj_t *cam_scr = lv_obj_create(NULL)` with an opaque black background; build the
  camera image + overlay onto `cam_scr`; `lv_screen_load(cam_scr)`. **The `lv_screen_load` is
  what closes the startup gap** — the old menu stops being active the instant capture begins, so
  the warm-up window shows black instead of the screen that led into the preview.
- On stop: destroy the pipeline/overlay but **leave `cam_scr` (now black) as the active screen**.
  The previous menu is no longer active, so it can't be revealed. When the host's next `View`
  loads its screen, `cam_scr` is cleaned up in the normal path.

Benefits: closes the **startup** reveal (black from the first tick), the **teardown** reveal (the
black backdrop persists across the teardown→next-load gap for free), and the **entropy back-out
stale-frame** bug (§ below) — because loading the real menu screen forces a full-screen repaint
that overwrites the lingering camera pixels. On landscape panels the screen's own black background
also covers the gutters during capture, so the entropy path stops leaking and the scanner's
gutter-blanking becomes belt-and-suspenders rather than load-bearing.

Coordination note: decide ownership of the *original* previous screen. Once `cam_scr` is
loaded, the menu is no longer the "previous active screen" the app's cleanup path would delete,
so either delete it deliberately on camera start, or confirm the host tolerates it lingering
until its own next load. This is the one wrinkle that makes Option A a design decision rather
than a drop-in.

### Option B — persistent full-screen black backdrop (minimal)
Keep the occlude-on-active-screen approach, but at start add a **full-screen** opaque black
`lv_obj` as the bottom-most child of `s_screen` (covering the whole previous screen, not just the
gutters). On stop, tear down the camera image + overlay but **leave that backdrop in place**;
nothing deletes it until the host cleans up `s_screen` on the next `lv_screen_load`. The gap then
shows black instead of the menu.

Simpler (no screen-ownership change), but it accumulates leftover widgets on the old screen until
the host's next load cleans the whole tree, and it's a continuation of the same "paint over the
stale screen" workaround rather than removing the root cause.

### Option C — synchronous handoff (host-sequencing)
Have the host load the next screen as part of the same teardown step (before yielding back to the
LVGL event loop), so the previous screen never gets a repaint tick. Weakest option: it depends on
the caller always having a next screen ready synchronously and races the render interval; A or B
are more robust.

## Recommendation
**Builder:** Option A, applied to **both** `camera_scanner` and `camera_entropy`. It removes the
root cause (occlude-not-replace), so it fixes the **startup** reveal, the **teardown** reveal for
both pipelines, the entropy path's during-capture gutter leak, and the entropy back-out stale
frame — all in one move.

**Main app (separate, host-side):** on cancel/back out of **either** camera flow — scanner *and*
entropy — honor the View-layer contract: close the current View, forward to the destination
(the previous View), and **run that destination View again from the top as if for the first
time**. Do not special-case a repaint or push view-reloading down into the builder/screens code;
re-running the View is what keeps state clean and isolated. This is the app's existing navigation
responsibility and the general navigation invariant, not a camera-only fix. Option A is what makes
it "just work": once the camera has its own screen, the menu was genuinely replaced, so the app's
ordinary back navigation re-runs the destination View correctly.

## Touch points
Builder (Option A):
- `ports/esp32/camera_scanner/camera_scanner.cpp` — `cam_scanner_start()` / `cam_scanner_stop()`
- `ports/esp32/camera_entropy/camera_entropy.cpp` — `cam_entropy_start()` / `cam_entropy_stop()`
- `seedsigner-lvgl-screens` overlay is unchanged — the gutter-blanking stays as-is (harmless
  belt-and-suspenders under Option A).

Main app (re-run the destination View on cancel/back — both flows):
- `seedsigner/` host navigation — backing out of **either** the camera-scanner or camera-entropy
  flow must close the current View and re-run the destination (previous) View from the top, per
  the View-layer contract, rather than assuming the menu is still displayed. The occlude-not-
  replace bug is what made that assumption wrong; fixing the builder side restores it, but re-
  running the View is the app's job and the general navigation invariant.

## Related docs
- `docs/camera-pipeline-integration-plan.md` — overall pipeline → LVGL → MicroPython plan
- `docs/camera-pipeline-phase2-poll-contract.md` — host poll/lifecycle contract
