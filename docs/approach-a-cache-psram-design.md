# Approach A — route the tiny_ttf cache index to PSRAM (design)

_Status: design + implementation in progress. Companion to `docs/font-memory-plan.md` (the
investigation + decision record) and `docs/knowledge/cjk-glyph-cache-pool-overflow.md` (root cause).
This doc owns the **how** of the agreed fix._

## Problem (one paragraph)

Realistic CJK navigation overflows **and structurally fragments** the 128 KB internal LVGL pool on the
P4-43. The fragmentation is permanent: TLSF coalesces adjacent frees but cannot compact, and the
long-lived per-`(font,px)` glyph-cache **index nodes** are interleaved with transient widgets. Every
cheap lever (cap cut, dedup, kerning-cache off, pool bump, flush-at-safe-points) was swept on-device and
proven insufficient — see the decision record in `font-memory-plan.md`. The only structural fix is to
keep the cache index nodes **out of the internal pool**: route them to PSRAM. The widget-churn isolation
test is positive proof this works — the pool defragments perfectly when only widgets occupy it, which is
what it becomes once the cache nodes move out.

## Where the memory actually lives (the load-bearing facts)

tiny_ttf builds its caches through the **generic** LVGL cache stack. Each cached glyph is three
allocations, made two layers below tiny_ttf:

| # | What | Allocated in | Size | Frees in |
|---|------|--------------|------|----------|
| 1 | `lv_rb_node_t` struct (tree node) | `lv_rb.c:335` `rb_create_node` | ~16 B | `lv_rb.c` (`lv_free(node)`) |
| 2 | `node->data` block — embeds the `lv_cache_entry_t` header, the cached glyph-data struct, **and** the LRU back-pointer (sized `lv_cache_entry_get_size(node_size) + sizeof(void*)`, see `lv_cache_lru_rb.c:215`) | `lv_rb.c:341` `rb_create_node` | ~40–50 B | **`lv_cache_entry.c:170`** `lv_cache_entry_delete` **and** `lv_rb.c` (`lv_rb_destroy`/`drop`) |
| 3 | LRU linked-list node (`lv_ll`, parallel LRU ordering) | `lv_cache_lru_rb.c:167` via generic `lv_ll` | ~12 B | `lv_cache_lru_rb.c` (`lv_free(lru_node)`) |

The dominant, **variable-size**, fragment-driving tenants are #1 and #2 (~56 B/entry/role, measured as
~145 B/glyph/role → ~290 B per distinct glyph across the active roles). #3 is small and **uniform-size**
(TLSF reuses uniform blocks from its segregated free list with little fragmentation).

tiny_ttf has **no lever** to redirect any of these: it passes a hardcoded cache class
(`&lv_cache_class_lru_rb_count`, `lv_tiny_ttf.c:409/417`) and data-only ops callbacks
(`compare`/`create`/`free` — no allocator hook; `lv_cache_ops_t` in `lv_cache_private.h`). The node
memory is owned by `lv_rb` / `lv_ll`, which `lv_malloc` unconditionally.

## Why patch `lv_rb`, not `lv_tiny_ttf`

We are patching the fetched LVGL managed component either way (same as Task C's stb-scratch patch, which
*does* live in `lv_tiny_ttf.c`). The question is only **which file holds the allocation we need to move**:

- The cache **node** allocation is in `lv_rb.c`, two layers below tiny_ttf. To redirect it *from* tiny_ttf
  we would have to invent a flag or custom cache class and thread it through
  `lv_cache.c → lv_cache_lru_rb.c → lv_rb.c` — a **4-file** patch — because even a custom class's
  `add_cb` bottoms out in `lv_rb_insert → rb_create_node`'s `lv_malloc`.
- Patching `lv_rb.c` hits the exact `lv_malloc` site with a **1-file** alloc change.

The only thing tiny_ttf-scoping would buy is *narrowing* the redirect to fonts. On the P4 build that
narrowing protects nothing: `lv_rb`'s only users are the **cache subsystem** (tiny_ttf glyph + draw-data
caches, plus the image / image-header decode caches) and the **OpenGL shader manager** — and we do not
build OpenGL/GLES on the P4. The image caches moving to PSRAM is fine, even beneficial (their lookups are
cold, not per-frame). So tiny_ttf-scoping costs 3 extra patched files for zero practical benefit, and the
maintainability criterion (smallest, most stable re-base surface) decides it: **redirect at the
allocation site.**

## The design (2 files)

The naive "patch `lv_rb.c` only" is **wrong**: block #2 is freed in `lv_cache_entry.c:170`
(`lv_cache_entry_delete → lv_free(data)`), *outside* `lv_rb.c`. Allocating it from PSRAM but freeing it
with `lv_free` (which only knows the internal TLSF pool) corrupts the heap. Chasing every scattered free
site is fragile.

Instead, split the change by concern:

### Patch 1 — `src/stdlib/lv_mem.c`: make `lv_free` PSRAM-aware (the free side, universal)

Add a guard at the top of `lv_free` (before `lv_free_core(data)`, ~line 136):

```c
if(data == NULL) return;
/* --- SEEDSIGNER PSRAM cache routing (Approach A) ----------------------------
 * rb/cache nodes routed to PSRAM are not in the LVGL TLSF pool; lv_free_core
 * would corrupt. Any external-RAM pointer reaching lv_free is one of ours
 * (nothing else lv_malloc()s into PSRAM), so route it to the IDF allocator.
 * See lv_rb.c + docs/approach-a-cache-psram-design.md. */
if(lv_rb_psram_owns(data)) { lv_rb_psram_note_free(data); return; }
/* --- end SEEDSIGNER --------------------------------------------------------- */
lv_free_core(data);
```

This makes **every** free path in all of LVGL correct — including `lv_cache_entry_delete`,
`lv_cache_lru_rb`, and any future path — without enumerating them. `lv_rb_psram_owns()` is a thin
`esp_ptr_external_ram()` wrapper; `lv_rb_psram_note_free()` does the accounting + `heap_caps_free()`.
Both are defined in `lv_rb.c` so the ESP-IDF includes stay localized; `lv_mem.c` only declares them
`extern`. (`lv_realloc` is **not** guarded: rb nodes are never realloc'd; documented assumption.)

### Patch 2 — `src/misc/lv_rb.c`: route the node allocation to PSRAM + instrumentation (the alloc side, scoped)

Change **only** the two allocations in `rb_create_node` (lines 335, 341) from `lv_malloc_zeroed(sz)` to
`ss_rb_psram_alloc(sz)`. The frees in `lv_rb.c` are left untouched — they call `lv_free`, which Patch 1
now handles. `ss_rb_psram_alloc`:

```c
static void *ss_rb_psram_alloc(size_t sz) {
    if(s_rb_psram.enabled) {
        void *p = heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM);   /* zeroed, like lv_malloc_zeroed */
        if(p) { s_rb_psram.alloc_total++; s_rb_psram.live_nodes++; s_rb_psram.live_bytes += sz; return p; }
        s_rb_psram.fallback_total++;   /* PSRAM exhausted -> fall back to the pool (still correct) */
    }
    return lv_malloc_zeroed(sz);
}
```

`lv_rb.c` also owns the instrumentation block: a `s_rb_psram` stats struct, the runtime `enabled` flag
(default **on**), `lv_rb_psram_owns()`, `lv_rb_psram_note_free()` (decrements live counters via
`heap_caps_get_allocated_size()` then `heap_caps_free()`), and the public getter/setter
(`lv_rb_psram_get_stats()`, `lv_rb_psram_set_enabled()`, `lv_rb_psram_is_enabled()`).

### Why this is the most maintainable shape

- **2 files, both minimal.** Patch 1 is a 3-line guard; Patch 2 is a 2-line alloc swap plus a clearly
  bannered instrumentation block. The touched functions (`lv_free`, `rb_create_node`) are among the most
  stable in LVGL.
- **Correct by construction.** Free correctness is centralized in one guard, not spread across every
  cache/rb free site we'd otherwise have to find and re-find on each LVGL bump.
- **Scoped where it matters, universal where it's safe.** PSRAM *allocation* is scoped to rb nodes; PSRAM
  *freeing* is universal and harmless for non-PSRAM pointers.
- **Collapses cleanly later.** If upstream ever exposes a per-cache allocator hook, Patch 2 becomes a
  callback registration and Patch 1 can stay as a belt-and-suspenders guard. Re-base checklist per bump:
  confirm `rb_create_node` still has the two `lv_malloc_zeroed` calls and `lv_free` still funnels through
  one function.

## Residual + staging

This moves tenants #1 and #2 (the variable-size fragmenters) to PSRAM. Tenant #3 (the ~12 B uniform LRU
`lv_ll` node) and the one-time `lv_lru_rb_t_` cache struct (~96 B × #caches) remain in the internal pool.
Uniform-size blocks fragment far less, so we expect the structural fragmentation to vanish. **Stage 1 is
rb-only; measure; only chase the `lv_ll` node (a scoped change in `lv_cache_lru_rb.c`, not a global
`lv_ll` patch) if fragmentation persists.** We will not patch generic `lv_ll` — it is the engine-wide
linked list (timers, anims, refresh lists).

**Result (validated 2026-06-26): Stage 1 sufficed.** Across the full 61-screen `zh_Hans_CN` navigation,
fragmentation stayed ≤16% and `biggest-free` held ≥65 KB — the uniform LL-node residual did **not**
fragment the pool, exactly as predicted. Stage 2 (scoping the `lv_ll` node) is **not needed.**

## Instrumentation (debugging is a first-class requirement)

- **Counters** (`lv_rb_psram_get_stats`): `alloc_total`, `live_nodes`, `live_bytes`, `fallback_total`,
  `free_total`, `enabled`. Surfaced through the existing `mem_stats()` MicroPython dict (Task D) as
  `rb_psram_*` keys alongside `lvgl_*` / `spiram_*` / `internal_*`. This directly answers "are the cache
  nodes actually in PSRAM, and is the route healthy (fallbacks == 0)?" while `lvgl_max_used` /
  `lvgl_frag_pct` should now stay flat under CJK and `spiram_min_free` absorb the load.
- **Runtime A/B toggle** (`seedsigner_lvgl.set_cache_psram(bool)` → `lv_rb_psram_set_enabled`): flip
  routing **off** early in a test script to reproduce the original in-pool overflow as a *control*, then
  **on** for the fix — both on one firmware. This is also how we validate the one open risk (below)
  without two builds.
- **Boot log**: `display_manager` init emits `ESP_LOGI` confirming the route is active and its enabled
  state, mirroring `route_draw_buffers_to_psram()`.
- **Fallback warning**: a rate-limited `ESP_LOGW` on the first PSRAM-alloc fallback (the fix silently
  degrading to the pool is exactly the kind of thing that must not be invisible).
- Concurrency note: counters are plain `volatile` and the `enabled` flag is a plain `bool`; allocs run on
  the LVGL task, frees mostly there but cache-drop can be driven from the MP task (e.g. `unload_locale`).
  Minor counter races are acceptable for debug stats; the `bool` read is atomic.

## Open risk — VALIDATED 2026-06-26

Per-glyph RB traversal now walks nodes **in PSRAM** on the animation/scroll hot path. PSRAM is higher
latency than internal SRAM. This was the plan's one standing concern about Approach A.

**On-device result:** the long **Advanced Settings** scrolling list (the longest scrollable list in the
currently-ported UI) rendered and scrolled with **no perceptible regression** vs the pre-A build, and
real-UI navigation throughout was smooth. So the PSRAM index walk does not penalize the scroll hot path
in practice. Residual lever if a future screen ever shows it: keep the small glyph cache warm; the
bitmaps are already in PSRAM, so this only adds the index walk. **Remaining future re-check:** the
heaviest not-yet-ported screens (long monospace address lists) — revisit when they land, but the
representative long list shows no penalty.

## Delivery — patching a fetched managed component

LVGL 9.5.0 is an ESP-IDF **managed component** fetched into the gitignored
`deps/micropython/upstream/ports/esp32/managed_components/lvgl__lvgl/`. The repo's normal source-patch
flow (`apply_micropython_mods.sh`) only diffs `ports/` and runs **before** the component is fetched, so it
cannot reach LVGL. There is no prior precedent for patching a managed component here — this establishes
the pattern. Mechanism:

1. **Carry the diffs** as re-baseable unified patches under `deps/micropython/mods/component_patches/`:
   - `lvgl-9.5.0-lv_mem-psram-free.patch` (Patch 1)
   - `lvgl-9.5.0-lv_rb-cache-psram.patch` (Patch 2)
2. **`scripts/apply_component_patches.sh`** applies every `component_patches/*.patch` to the materialized
   component **idempotently** — guarded by a sentinel grep (e.g. `SEEDSIGNER PSRAM` in the target file):
   present ⇒ skip, absent ⇒ apply. This tolerates both re-runs and a re-fetched (pristine) component.
3. **Timing hook**: call it from `build_firmware.sh` **between** the `make … submodules` step (line ~106,
   whose reconfigure triggers the IDF component manager and materializes `managed_components/`) and the
   real `make … -j` build (line ~107). That is the "after fetch, before compile" window. The component
   persists in the source tree across normal builds (`rm -rf build` does not remove it), so the common
   case is "already present"; the script guards on existence and fails loudly rather than silently
   shipping an unpatched build.
4. **Idempotent / re-fetch safe.** Because the sentinel check drives application, a clean re-fetch
   (pristine source) is re-patched and an already-patched tree is skipped.

`generate_micropython_patch.sh` is unaffected — it diffs `ports/` against the pinned submodule commit and
never sees the gitignored `managed_components/`. These component patches are hand-maintained, separate
from the `0001` MicroPython patch series, and regenerated from a pristine-vs-edited `diff -u`.

Future hardening (not Stage 1): a CMake-level sentinel assertion in `esp32_common.cmake` (via the `0001`
patch) that errors if `lv_rb.c` lacks the sentinel — catching any build path that bypasses
`build_firmware.sh`. Documented here; deferred until the patch itself is validated.

## Results (on-device, P4-43, 2026-06-26)

Built clean (firmware + host screenshot-gen); all `lv_rb_psram_*` / `dm_*` symbols linked. On-device via
`repro_realistic_nav.py` (`zh_Hans_CN`, guard 78%/22 KB) and real-UI navigation:

| Metric | Pre-A (kerning=0 + dedup + cap256) | **Approach A** |
|---|---|---|
| `seed_views.py` complete | 78% / `max_used` 77,984 | 24% / **37,924** |
| Full 61-screen navigation | **crashed ~screen 30/61** | **completed 61/61** |
| Peak `max_used` | ~110 K (90%, then reboot) | **56,660 (44%)** |
| Fragmentation peak | 64–67% | **16%** |
| `biggest-free` floor | collapsed to 4.5 KB | held **≥65,640 B** |
| Long Advanced-Settings scroll | n/a | **smooth, no regression** |

The cache index left the pool (the ~40 KB `seed_views` drop), `max_used` plateaus instead of climbing
linearly, `biggest-free` stays flat (the fragmentation fix), and the scroll hot path is unaffected.
Overflow + structural fragmentation **resolved**; latency risk **cleared** for the current UI.

### Comprehensive multi-language + animation stress (2026-06-26)

`tools/stress_locale_churn.py` drives **all 11 locales × 2 passes** through **every screen type**
(button_list, large_icon_status, passphrase keyboard in all 3 pages, localized main_menu, screensaver),
realistic shapes (paragraphs in text fields, the full localized settings list as a long button_list),
and an **animation torture block** (the 4 status types with pulsing `warning_edges`, an autoscrolling
top_nav title + headline, all dwelled ~2 s so the esp_lvgl_port task actually runs the animations and
re-traverses the glyph cache in PSRAM). Result — **clean across the board**:

- **No crash, `rb_psram_fallback == 0`, zero render failures** on every locale/screen.
- **Flat high-water / converged:** `max_used` moved **59,100 → 59,520 (+420 B)** across the entire second
  pass; the heavy CJK locales' peak `used%` is within ~1 % pass-to-pass (zh 40/41, ja 42/43, fa 39/39,
  th 42/43). Peak ~43 % (`max_used` ≈ 46 % of the 128 KB pool); `biggest-free` never below ~65 KB.
- **Free path proven under churn:** switching locales reaps the prior language's cache nodes through
  `lv_cache_entry_delete → lv_free → the PSRAM guard`; `rb_psram_live_nodes` returns to ~baseline on
  `unload_locale()`. (An earlier run showed ~6.5 KB/pass internal-pool growth; isolating it confirmed the
  cause was the **screensaver's legacy `lv_scr_load`** orphaning one screen/pass — harness-only, the app
  uses the timeout/save-restore path — not a cache-node leak. Moving the screensaver out of the per-pass
  loop dropped the growth to +420 B.)

So Approach A holds under the most exhaustive realistic + animation-heavy, all-language load we could
build: the cache index lives in PSRAM, the free path converges, and the internal pool stays at <half
capacity. Diagnostic methodology note (the large-script compile trap hit while building this harness):
`docs/knowledge/micropython-large-script-const-dedup-wdt.md`.

## Verification plan

1. **Build** P4-43 (and an S3 board) via `BOARD=… make docker-build-all`; confirm
   `apply_component_patches.sh` applied both patches (sentinel present) and a clean rebuild re-applies
   idempotently.
2. **Route active**: `mem_stats()['rb_psram_alloc_total'] > 0`, `rb_psram_fallback_total == 0`,
   `spiram_min_free` drops under a first-paint CJK screen; `lvgl_max_used` no longer climbs ~290 B/glyph.
3. **Budget / fragmentation**: drive the Task-E worst path (`tools/repro_realistic_nav.py`, guard restored
   to 78/22000) through the full 61-screen navigation; `lvgl_max_used` ≤ target with margin, `frag_pct`
   stays low, `free_biggest` does not collapse, **no freeze/reboot**. Capture the locale-switch transient.
   HARD-RESET between runs (`ss.init()` does not clear the pool).
4. **Control comparison**: same path with `set_cache_psram(False)` reproduces the original overflow —
   proves the delta is the routing.
5. **Hot-path latency**: `tools/timing_compare.py` + webcam scroll/animation A/B (toggle on vs off);
   confirm acceptable.
6. **Functional**: webcam-verify CJK + a shaping locale (`ur`) still render (`/verify`, `/hw-debug`).
7. Keep dedup + `CONFIG_LV_TINY_TTF_CACHE_KERNING_CNT=0` throughout (orthogonal "keep" levers).

## Relationship to Task C (stb scratch)

Separate allocation, separate file. Task C redirects tiny_ttf's stb rasterization **scratch**
(`lv_tiny_ttf.c` L27–28 `STBTT_malloc/STBTT_free`) to PSRAM. Approach A redirects the cache **index
nodes** (`lv_rb.c`). Both are managed-component patches delivered by the same
`apply_component_patches.sh` hook; do them together once the hook lands.
