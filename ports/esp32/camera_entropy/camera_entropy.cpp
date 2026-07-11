/**
 * camera_entropy — builder-side composition of the image-entropy capture flow
 * (see camera_entropy.h for the contract). Owns pipeline + entropy consumer as a
 * single-session singleton; the MicroPython bindings are a thin skin over the
 * cam_entropy_* C API below. Mirrors camera_scanner, minus the overlay/coordinator.
 */
#include "camera_entropy.h"

#include <string.h>

#include "board.h"
#include "board_config.h"

#include "esp_log.h"

static const char *TAG = "camera_entropy";

/* Partition mode (P4-35 ST7796): LVGL keeps running during the preview. Default
 * off for boards that use the LVGL-stopped dummy-draw path. */
#ifndef BOARD_CAMERA_PARTITION_MODE
#define BOARD_CAMERA_PARTITION_MODE 0
#endif

#if BOARD_HAS_CAMERA

/* The engine headers lack an extern "C" guard. Force C linkage HERE, before any
 * board header (board_pipeline.h) pulls esp_cam_pipeline.h in under C++ linkage —
 * once that happens its include guard would defeat a later wrap and leave
 * cam_pipeline_create() C++-mangled (link error). Same ordering as camera_scanner. */
extern "C" {
#include "esp_cam_pipeline.h"
#include "cam_pipeline_entropy.h"
}

#include "board_pipeline.h"
#include "board_i2c.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "camera_entropy_overlay.h"  /* seedsigner-lvgl-screens overlay (LVGL) */

/* Single live session: one camera, one entropy consumer, tied to start()/stop(). */
static cam_pipeline_handle_t          s_pipeline = NULL;
static cam_pipeline_entropy_handle_t  s_entropy  = NULL;
static camera_entropy_overlay_t      *s_overlay  = NULL;
static lv_obj_t                      *s_screen   = NULL;
static bool                           s_running  = false;
static bool                           s_await_confirm = false;  /* capture()→CONFIRM latch */
static volatile uint32_t              s_frames   = 0;  /* live progress counter */
static uint8_t                        s_seed[32];      /* copy of optional seed */

/* Overlay strings are HOST-PROVIDED + already localized (cam_entropy_set_labels, called
 * by the app's gettext BEFORE start). Nothing is hardcoded here; empty until set. The
 * shutter's camera icon is a symbol supplied by the overlay itself. */
static char s_capturing_text[80] = {0};  /* CAPTURING transient, e.g. "Capturing image..." */
static char s_accept_label[40]   = {0};  /* CONFIRM accept button, e.g. "Accept" */

/* Flip the overlay phase under the LVGL lock (host thread calls capture/resume/etc). */
static void overlay_set_phase(camera_entropy_phase_t phase)
{
    if (s_overlay && lvgl_port_lock(0)) {
        camera_entropy_overlay_set_phase(s_overlay, phase);
        lvgl_port_unlock();
    }
}

/* ── Option A dedicated camera screen ───────────────────────────────────────────
 * The camera image renders onto its OWN opaque-black screen rather than over the
 * previously-active one, so the warm-up window (and the teardown→next-load gap)
 * shows black instead of the screen the user came from. This matters more here than
 * for the scanner: camera_entropy has NO overlay, so on a landscape panel its own
 * black background is also the only thing covering the side gutters during capture.
 * See docs/camera-pipeline-teardown-screen-reveal.md. ── */

/* Create the black cam screen and load it. *out_prev receives the screen that was
 * active before the swap; the caller deletes it on success (the destination View
 * rebuilds it fresh on return, per the View-layer contract) or restores it on a
 * start failure. Returns NULL on alloc failure. Caller must hold the LVGL lock. */
static lv_obj_t *cam_make_black_screen(lv_obj_t **out_prev)
{
    lv_obj_t *prev = lv_screen_active();
    if (out_prev) {
        *out_prev = prev;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    if (!scr) {
        return NULL;
    }
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);
    return scr;
}

/* Undo the start-time screen swap when a later start step fails: revert the render
 * interval, restore the pre-camera screen, and drop our black one, so the caller
 * sees exactly the state it had before start() was attempted. */
static void cam_rollback_screen(lv_obj_t *prev_screen)
{
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        if (prev_screen) {
            lv_screen_load(prev_screen);
        }
        if (s_screen) {
            lv_obj_delete(s_screen);
            s_screen = NULL;
        }
        lvgl_port_unlock();
    }
}

/* Fires once per chained preview frame on the consumer task — just record the
 * count for cam_entropy_frames_chained() (must return quickly). */
static void on_entropy_frame(uint32_t frames_chained, void *ctx)
{
    (void)ctx;
    s_frames = frames_chained;
}

static void copy_label(char *dst, size_t cap, const char *src)
{
    if (src && src[0]) {
        strncpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

void cam_entropy_set_labels(const char *capturing_text, const char *accept_label)
{
    copy_label(s_capturing_text, sizeof(s_capturing_text), capturing_text);
    copy_label(s_accept_label,   sizeof(s_accept_label),   accept_label);
}

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }
    if (seed_hash && seed_len != 0 && seed_len != 32) {
        return "seed_hash must be 32 bytes";
    }

    /* Build on our OWN opaque-black screen (Option A) rather than the active one, so
     * the warm-up window shows black instead of the screen the user came from (the
     * camera image renders as its child). Short render interval for a responsive
     * real-time preview, matching camera_scanner / the standalone camera apps; reset
     * to 0 in stop. prev_screen is deleted on success / restored on failure below. */
    lv_obj_t *prev_screen = NULL;
    if (lvgl_port_lock(0)) {
        s_screen = cam_make_black_screen(&prev_screen);
#if !BOARD_CAMERA_PARTITION_MODE
        /* Partition mode drives chrome off LVGL's own refresh timer; the kick is
         * only needed by the image-widget / legacy dummy-draw paths. */
        if (s_screen) {
            board_set_render_interval_ms(10);
        }
#endif
        lvgl_port_unlock();
    }
    if (!s_screen) {
        return "camera screen create failed";
    }

    /* Camera preview pipeline: centered square = the shorter logical dimension
     * (same geometry as camera_scanner). cam_pipeline_create manages its own LVGL
     * access, so it runs OUTSIDE the lock. */
    cam_pipeline_config_t pcfg =
        board_pipeline_default_config(s_screen, board_i2c_get_handle());
    uint32_t square = (BOARD_DISP_H_RES < BOARD_DISP_V_RES)
                          ? BOARD_DISP_H_RES : BOARD_DISP_V_RES;
    pcfg.display_width  = square;
    pcfg.display_height = square;

    s_pipeline = cam_pipeline_create(&pcfg);
    if (!s_pipeline) {
        cam_rollback_screen(prev_screen);
        return "pipeline create failed";
    }

    /* Copy any caller seed so the consumer config can point at stable storage. */
    const uint8_t *seed = NULL;
    if (seed_hash && seed_len == 32) {
        memcpy(s_seed, seed_hash, 32);
        seed = s_seed;
    }

    s_frames = 0;
    cam_pipeline_entropy_config_t ecfg = {};
    ecfg.pipeline     = s_pipeline;
    ecfg.frame_width  = square;
    ecfg.frame_height = square;
    ecfg.seed_hash    = seed;
    ecfg.on_frame     = on_entropy_frame;
    ecfg.user_ctx     = NULL;
    s_entropy = cam_pipeline_entropy_create(&ecfg);
    if (!s_entropy) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "entropy consumer create failed";
    }

    /* Overlay over the camera image (created after the pipeline's LVGL image so it draws
     * above it), on our black cam screen. Same centered-square geometry as the camera
     * image / the camera_scanner overlay. Strings are host-provided (empty until the app
     * calls cam_entropy_set_labels); the shutter's camera icon is supplied by the overlay.
     * PREVIEW phase to start. */
    int32_t sq_x = (BOARD_DISP_H_RES - (int32_t)square) / 2;
    int32_t sq_y = (BOARD_DISP_V_RES - (int32_t)square) / 2;
    s_await_confirm = false;
    if (lvgl_port_lock(0)) {
        camera_entropy_overlay_spec_t spec = {};
        spec.square_x        = sq_x;
        spec.square_y        = sq_y;
        spec.square_w        = (int32_t)square;
        spec.square_h        = (int32_t)square;
        spec.capturing_text  = s_capturing_text[0] ? s_capturing_text : NULL;
        spec.accept_label    = s_accept_label[0]   ? s_accept_label   : NULL;
        spec.capture_icon    = NULL;  /* overlay defaults to the camera glyph */
        spec.capture_style   = CAMERA_ENTROPY_CAPTURE_RING;
        spec.phase           = CAMERA_ENTROPY_PHASE_PREVIEW;
        s_overlay = camera_entropy_overlay_create(s_screen, &spec);
        lvgl_port_unlock();
    }
    if (!s_overlay) {
        cam_pipeline_entropy_destroy(s_entropy);
        s_entropy = NULL;
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "overlay create failed";
    }

    /* Success: the pre-camera screen is no longer needed — the destination View
     * rebuilds it fresh when we return (View-layer contract), and the host's
     * load_screen_and_cleanup_previous only reaps the *active* screen (now ours), so
     * this orphan would otherwise leak. Delete it deliberately here; our black cam
     * screen stays active for the whole session and is itself reaped by the host's
     * next screen load. */
    if (prev_screen && prev_screen != s_screen) {
        if (lvgl_port_lock(0)) {
            lv_obj_delete(prev_screen);
            lvgl_port_unlock();
        }
    }

    s_running = true;
    ESP_LOGI(TAG, "entropy capture started (%ux%u square)", (unsigned)square, (unsigned)square);
    return NULL;
}

void cam_entropy_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* Consumer first (it stops the hashing task + unfreezes the pipeline if a
     * capture left it frozen, and zeroizes its chain/latch). */
    if (s_entropy) {
        cam_pipeline_entropy_destroy(s_entropy);
        s_entropy = NULL;
    }
    /* Overlay handle next — frees ONLY the handle struct; its widgets belong to the
     * screen tree and are stripped below (lv_obj_clean). Then the pipeline (deletes the
     * camera image). */
    if (s_overlay) {
        if (lvgl_port_lock(0)) {
            camera_entropy_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    /* Revert the render interval; and strip any residual overlay widgets so the black cam
     * screen is PURE black through the teardown→next-load gap (Option A). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        if (s_screen) {
            lv_obj_clean(s_screen);
        }
        lvgl_port_unlock();
    }
    memset(s_seed, 0, sizeof(s_seed));
    s_frames = 0;
    s_await_confirm = false;
    /* Option A: leave our black cam screen loaded as the active screen — camera image +
     * overlay widgets gone, it shows black, covering the teardown→next-load gap. We drop
     * our reference WITHOUT deleting the object; the host reaps it via
     * load_screen_and_cleanup_previous when the destination View re-runs. */
    s_screen = NULL;
    ESP_LOGI(TAG, "entropy capture stopped");
}

bool cam_entropy_is_running(void)
{
    return s_running;
}

uint32_t cam_entropy_frames_chained(void)
{
    return s_frames;
}

void cam_entropy_capture(void)
{
    if (s_entropy) {
        cam_pipeline_entropy_capture(s_entropy);
    }
    /* Show the "Capturing…" transient; get_result() flips to CONFIRM once the frozen
     * frame is latched (the overlay's shutter/back hide, so no double-capture). */
    s_await_confirm = true;
    overlay_set_phase(CAMERA_ENTROPY_PHASE_CAPTURING);
}

bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained)
{
    if (!s_entropy) {
        return false;
    }
    bool ok = cam_pipeline_entropy_get_result(s_entropy, chain, chain_len,
                                              frame, frame_len, frames_chained);
    /* First successful latch after capture(): advance CAPTURING → CONFIRM (accept /
     * reshoot). One-shot so repeated polls don't re-trigger. */
    if (ok && s_await_confirm) {
        s_await_confirm = false;
        overlay_set_phase(CAMERA_ENTROPY_PHASE_CONFIRM);
    }
    return ok;
}

void cam_entropy_resume(void)
{
    if (s_entropy) {
        cam_pipeline_entropy_resume(s_entropy);
    }
    /* Reshoot: back to the live PREVIEW (shutter + cancel). */
    s_await_confirm = false;
    overlay_set_phase(CAMERA_ENTROPY_PHASE_PREVIEW);
}

#else /* !BOARD_HAS_CAMERA — bindings still link; start() reports the absence. */

const char *cam_entropy_start(const uint8_t *seed_hash, size_t seed_len) { (void)seed_hash; (void)seed_len; return "board has no camera"; }
void cam_entropy_stop(void) {}
void cam_entropy_set_labels(const char *capturing_text, const char *accept_label) { (void)capturing_text; (void)accept_label; }
bool cam_entropy_is_running(void) { return false; }
uint32_t cam_entropy_frames_chained(void) { return 0; }
void cam_entropy_capture(void) {}
bool cam_entropy_get_result(const uint8_t **chain, size_t *chain_len,
                            const uint8_t **frame, size_t *frame_len,
                            uint32_t *frames_chained) {
    (void)chain; (void)chain_len; (void)frame; (void)frame_len; (void)frames_chained;
    return false;
}
void cam_entropy_resume(void) {}

#endif /* BOARD_HAS_CAMERA */
