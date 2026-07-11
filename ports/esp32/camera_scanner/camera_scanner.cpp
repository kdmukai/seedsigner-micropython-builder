/**
 * camera_scanner — builder-side composition of the QR scan pipeline (see
 * camera_scanner.h for the contract). Owns pipeline + overlay + coordinator as a
 * single-session singleton; the MicroPython bindings are a thin skin over the
 * cam_scanner_* C API below.
 */
#include "camera_scanner.h"

#include <string.h>

#include "board.h"
#include "board_config.h"

#include "esp_log.h"

static const char *TAG = "camera_scanner";

/* Partition mode (P4-35 ST7796): LVGL keeps running during the preview so the
 * side gutters render live chrome with working touch. Default off for boards
 * that use the LVGL-stopped dummy-draw path. */
#ifndef BOARD_CAMERA_PARTITION_MODE
#define BOARD_CAMERA_PARTITION_MODE 0
#endif

#if BOARD_HAS_CAMERA

/* The engine headers lack an extern "C" guard. Force C linkage HERE, before any
 * board header (board_pipeline.h) pulls esp_cam_pipeline.h in under C++ linkage —
 * once that happens its include guard would defeat a later wrap and leave
 * cam_pipeline_create() C++-mangled (link error). Same ordering as qr_overlay_test. */
extern "C" {
#include "esp_cam_pipeline.h"
#include "cam_pipeline_qr.h"
}
#include "scan_coordinator.h"

#include "board_pipeline.h"
#include "board_i2c.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "camera_preview_overlay.h"  /* seedsigner-lvgl-screens overlay (LVGL) */

/* Single live session. The pipeline, overlay, and coordinator have one-of-each
 * lifetimes tied to start()/stop(); there is only ever one camera. */
static cam_pipeline_handle_t      s_pipeline = NULL;
static camera_preview_overlay_t  *s_overlay  = NULL;
static scan_coordinator_t        *s_coord    = NULL;
static lv_obj_t                  *s_screen   = NULL;
static bool                       s_running  = false;

#if BOARD_CAMERA_PARTITION_MODE
/* M1 partition-mode proof: a live progress widget in the right gutter (updated
 * every present()) demonstrating non-camera LVGL chrome rendering beside the
 * streaming square with LVGL running + touch alive. Throwaway scaffolding — M3
 * replaces it with the real gutter layout in camera_preview_overlay (screens). */
static lv_obj_t                  *s_gutter_bar = NULL;
static lv_obj_t                  *s_gutter_pct = NULL;
#endif

/* ── Option A dedicated camera screen ───────────────────────────────────────────
 * The camera image + overlay render onto their OWN opaque-black screen rather than
 * over the previously-active one. Loading it immediately means the camera warm-up
 * window (and, on stop, the teardown→next-load gap) shows black instead of the
 * screen the user came from — see docs/camera-pipeline-teardown-screen-reveal.md. ── */

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

/* ── Injected presenter: maps the neutral scan status to the overlay's visual
 * enum and drives the LVGL widgets. The coordinator already dedups on
 * (status, percent), so this fires only on a real change.
 *
 * THREADING: present() runs on the CONSUMER task (it is called from
 * scan_coordinator_report(), which the MicroPython consumer calls), NOT the decode
 * task. So unlike the old PoC bridge — which ran on the decode task and dropped on
 * lock contention to stay off the flush critical path — here we BLOCK on the lock
 * (timeout 0 = wait): the consumer task is decoupled from the camera/decode rate by
 * the ring, so a brief wait for the LVGL flush is lossless and correct. ── */
static void cam_present(void *ctx, int percent, scan_frame_status_t status)
{
    camera_preview_overlay_t *ov = (camera_preview_overlay_t *)ctx;
    if (!ov) {
        return;
    }

    camera_overlay_frame_status_t dot;
    switch (status) {
    case SCAN_FRAME_NEW:    dot = CAMERA_OVERLAY_FRAME_ADDED;    break;  /* green  */
    case SCAN_FRAME_REPEAT: dot = CAMERA_OVERLAY_FRAME_REPEATED; break;  /* gray   */
    case SCAN_FRAME_MISS:   /* hidden for now — sustained-MISS warning is Python's (§6) */
    case SCAN_FRAME_NONE:
    default:                dot = CAMERA_OVERLAY_FRAME_NONE;     break;  /* hidden */
    }

    if (lvgl_port_lock(0)) {
        camera_preview_overlay_set_progress(ov, percent, dot);
#if BOARD_CAMERA_PARTITION_MODE
        if (s_gutter_bar) lv_bar_set_value(s_gutter_bar, percent, LV_ANIM_OFF);
        if (s_gutter_pct) lv_label_set_text_fmt(s_gutter_pct, "%d%%", percent);
#endif
        lvgl_port_unlock();
    }
}

/* ── Completion (terminal, once). The coordinator emits the final NEW@100% present
 * just before this, so the bar is already full + green; this is just the terminal
 * marker. The Python consumer drives its own post-complete handoff (it called
 * report_complete), so nothing more is needed here than a log. ── */
static void cam_complete(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "scan complete");
}

#if BOARD_CAMERA_PARTITION_MODE
/* ── M1 gutter chrome (placeholder) ──────────────────────────────────────────
 * On-device proof of the partition mechanism: live, non-camera LVGL widgets in
 * the right gutter that keep updating (with the left-gutter back button
 * touchable) while the camera direct-blits the square. Throwaway scaffolding —
 * M3 replaces it with the real cancel/percent/vertical-bar gutter layout in the
 * camera_preview_overlay screen. Caller holds the LVGL port lock. */
static void cam_gutter_placeholder_create(lv_obj_t *parent, int32_t sq_x, int32_t square)
{
    int32_t rg_x = sq_x + square;                       /* right gutter left edge  */
    int32_t rg_w = (int32_t)BOARD_DISP_H_RES - rg_x;    /* right gutter width (~80) */
    if (rg_w <= 0) return;                              /* square fills the panel   */
    int32_t cx = rg_x + rg_w / 2;

    s_gutter_pct = lv_label_create(parent);
    lv_label_set_text(s_gutter_pct, "0%");
    lv_obj_set_style_text_color(s_gutter_pct, lv_color_white(), 0);
    lv_obj_align(s_gutter_pct, LV_ALIGN_TOP_LEFT, cx - 16, 12);

    int32_t bar_w = 20;
    int32_t bar_h = (int32_t)BOARD_DISP_V_RES - 60;     /* taller-than-wide => vertical */
    s_gutter_bar = lv_bar_create(parent);
    lv_obj_set_size(s_gutter_bar, bar_w, bar_h);
    lv_obj_set_pos(s_gutter_bar, cx - bar_w / 2, 44);
    lv_bar_set_range(s_gutter_bar, 0, 100);
    lv_bar_set_value(s_gutter_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_gutter_bar, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_gutter_bar, lv_color_hex(0xF08C00), LV_PART_INDICATOR);
}

static void cam_gutter_placeholder_destroy(void)
{
    if (s_gutter_bar) { lv_obj_delete(s_gutter_bar); s_gutter_bar = NULL; }
    if (s_gutter_pct) { lv_obj_delete(s_gutter_pct); s_gutter_pct = NULL; }
}
#endif /* BOARD_CAMERA_PARTITION_MODE */

const char *cam_scanner_start(void)
{
    if (s_running) {
        return NULL;  /* idempotent */
    }

    /* Build on our OWN opaque-black screen (Option A) rather than the active one, so
     * the camera warm-up window shows black instead of the screen the user came from
     * — the camera image + overlay become its children, so the overlay draws above
     * the live preview. Also set a short LVGL render interval for a responsive
     * real-time preview, matching the standalone camera apps (qr_overlay_test,
     * scan_coord_test); reset to 0 in cam_scanner_stop. (The overlay-lock starvation
     * that used to hang start() is fixed by flattening the lvgl/CSI task priorities
     * to 1 — see board_common.) prev_screen is deleted on success / restored on
     * failure below. */
    lv_obj_t *prev_screen = NULL;
    if (lvgl_port_lock(0)) {
        s_screen = cam_make_black_screen(&prev_screen);
#if !BOARD_CAMERA_PARTITION_MODE
        /* Image-widget / legacy dummy-draw paths want a responsive render kick.
         * Partition mode drives its gutter chrome off LVGL's own ~33ms refresh
         * timer instead, so the kick is dropped to keep the camera's blit lock
         * uncontended (the preview is direct-blit and never LVGL-driven). */
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
     * (the display sink centers it on the panel; landscape gutters stay static).
     * cam_pipeline_create manages its own LVGL access, so it runs OUTSIDE the lock
     * (same as qr_overlay_test). */
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

    /* Overlay over the camera image: created after the pipeline's LVGL image so it
     * draws above it. Geometry matches the centered square so the in-square status
     * bar sits over the camera and the gutter back button lands in the static
     * margin (never over live pixels). */
    int32_t sq_x = (BOARD_DISP_H_RES - (int32_t)square) / 2;
    int32_t sq_y = (BOARD_DISP_V_RES - (int32_t)square) / 2;
    if (lvgl_port_lock(0)) {
        camera_preview_overlay_spec_t spec = {};
        spec.instructions_text = "< back  |  Scan a QR code";  /* ignored in touch mode */
        spec.square_x = sq_x;
        spec.square_y = sq_y;
        spec.square_w = (int32_t)square;
        spec.square_h = (int32_t)square;
        spec.scanning_active = true;  /* show the status bar from the start */
        spec.progress_percent = 0;
        spec.frame_status = CAMERA_OVERLAY_FRAME_NONE;
        s_overlay = camera_preview_overlay_create(s_screen, &spec);
        lvgl_port_unlock();
    }
    if (!s_overlay) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "overlay create failed";
    }

    /* Coordinator: engine per-frame outcome -> transport-dedup -> NEW ring +
     * status cell; report() -> cam_present -> overlay. */
    scan_coordinator_config_t ccfg = {};
    ccfg.pipeline       = s_pipeline;
    ccfg.frame_width    = square;
    ccfg.frame_height   = square;
    ccfg.present        = cam_present;
    ccfg.present_ctx    = s_overlay;
    ccfg.on_complete    = cam_complete;
    ccfg.complete_ctx   = s_overlay;
    ccfg.new_ring_depth = 0;  /* default */
    s_coord = scan_coordinator_create(&ccfg);
    if (!s_coord) {
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
            lvgl_port_unlock();
        }
        s_overlay = NULL;
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
        cam_rollback_screen(prev_screen);
        return "scan_coordinator create failed";
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

#if BOARD_CAMERA_PARTITION_MODE
    /* Add the live gutter placeholder now that the session is fully built. */
    if (lvgl_port_lock(0)) {
        cam_gutter_placeholder_create(s_screen, sq_x, (int32_t)square);
        lvgl_port_unlock();
    }
#endif

    s_running = true;
    ESP_LOGI(TAG, "scanner started (%ux%u square)", (unsigned)square, (unsigned)square);
    return NULL;
}

void cam_scanner_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;

    /* Coordinator first: it stops the QR consumer (no more present() calls) before
     * we free the overlay it points at. It does NOT touch the pipeline. */
    if (s_coord) {
        scan_coordinator_destroy(s_coord);
        s_coord = NULL;
    }
    if (s_overlay) {
        if (lvgl_port_lock(0)) {
            camera_preview_overlay_destroy(s_overlay);
#if BOARD_CAMERA_PARTITION_MODE
            cam_gutter_placeholder_destroy();
#endif
            lvgl_port_unlock();
        }
        s_overlay = NULL;
    }
    if (s_pipeline) {
        cam_pipeline_destroy(s_pipeline);
        s_pipeline = NULL;
    }
    /* Revert the render interval to the idle default (set in cam_scanner_start). */
    if (lvgl_port_lock(0)) {
        board_set_render_interval_ms(0);
        lvgl_port_unlock();
    }
    /* Option A: leave our black cam screen loaded as the active screen — with the
     * camera image gone it shows black, covering the teardown→next-load gap so the
     * pre-camera screen is never revealed. We drop our reference WITHOUT deleting the
     * object; the host reaps it via load_screen_and_cleanup_previous when the
     * destination View re-runs and loads its own screen. */
    s_screen = NULL;
    ESP_LOGI(TAG, "scanner stopped");
}

bool cam_scanner_is_running(void)
{
    return s_running;
}

bool cam_scanner_poll_new(const uint8_t **payload, size_t *len)
{
    if (!s_coord || !payload || !len) {
        return false;
    }
    scan_new_event_t ev;
    if (!scan_coordinator_poll_new(s_coord, &ev)) {
        return false;
    }
    *payload = ev.payload;
    *len = ev.len;
    return true;
}

void cam_scanner_read_status(cam_scanner_status_t *out)
{
    if (!out) {
        return;
    }
    *out = cam_scanner_status_t{};
    if (!s_coord) {
        return;
    }
    scan_status_t st;
    scan_coordinator_read_status(s_coord, &st);
    out->latest             = (int)st.latest;
    out->consecutive_misses = st.consecutive_misses;
    out->dropped_new        = st.dropped_new;
    out->has_corners        = st.has_corners;  /* false until engine plumbs corners */
}

void cam_scanner_report(int status, int percent)
{
    if (!s_coord) {
        return;
    }
    scan_coordinator_report(s_coord, (scan_frame_status_t)status, percent);
}

void cam_scanner_report_complete(void)
{
    if (!s_coord) {
        return;
    }
    scan_coordinator_report_complete(s_coord);
}

#else /* !BOARD_HAS_CAMERA — bindings still link; start() reports the absence. */

const char *cam_scanner_start(void) { return "board has no camera"; }
void cam_scanner_stop(void) {}
bool cam_scanner_is_running(void) { return false; }
bool cam_scanner_poll_new(const uint8_t **payload, size_t *len) { (void)payload; (void)len; return false; }
void cam_scanner_read_status(cam_scanner_status_t *out) { if (out) { memset(out, 0, sizeof(*out)); } }
void cam_scanner_report(int status, int percent) { (void)status; (void)percent; }
void cam_scanner_report_complete(void) {}

#endif /* BOARD_HAS_CAMERA */
