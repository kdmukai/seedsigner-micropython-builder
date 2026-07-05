# Locale picker + endonym-image provider — the MicroPython builder half

_How the ESP32 firmware exposes the language-selection picker and runtime
language-pack discovery to the shared Python app. Written 2026-07-05 alongside the
builder-side wiring. The screens-layer half (why images-over-fonts, the "SSA8"
blob format, runtime manifest registration) is in the screens repo's
`docs/knowledge/locale-picker-and-endonym-images.md`; this doc is the builder-side
integration notes that aren't obvious from the code alone._

## What the builder adds

The native module `seedsigner_lvgl_screens` gained four Python entry points
(`bindings/modseedsigner_bindings.c`), each routed through a `dm_*` wrapper in
`ports/esp32/display_manager/display_manager.cpp`:

| Python binding | `dm_` wrapper | screens-layer call |
|---|---|---|
| `locale_picker_screen(cfg, endonym_images)` | `run_screen` + `dm_set_endonym_image_provider` | `locale_picker_screen()` / `locale_picker_set_image_provider()` |
| `register_pack_manifest(bytes) -> bool` | `dm_register_pack_manifest` | `ss_register_pack_manifest()` |
| `clear_pack_manifests()` | `dm_clear_pack_manifests` | `ss_clear_pack_manifests()` |
| `list_available_locales() -> str` | `dm_supported_locales_json` | `supported_locales_json()` |

The SD-card discovery + endonym staging + picker-cfg building are deliberately
**not** here — they are business logic that runs in the shared `seedsigner` app
(and are platform-branched there because `machine.SDCard` is ESP32-only). The
builder only provides the native seam.

## Non-obvious constraint 1 — the endonym provider must key by `(locale, file)`

The existing `mp_pack_provider` (used by `load_locale`) keys its staging dict by
**filename alone** and ignores `locale`. That is correct for `load_locale`:
exactly one locale is active, so `zh_Hans_CN.ttf` is unambiguous.

The picker breaks that assumption. It lists **every** onboard language's name on
one screen, fetching each language's endonym image during the build — and every
pack's endonym image is named identically, `endonym_<profile-height>.bin`
(`endonym_480.bin`, …). Keyed by filename alone they would all collide on the
first-staged blob.

So there is a **second provider**, `mp_endonym_provider`, that rebuilds a composite
key `"<locale>/<file>"` (e.g. `"hi/endonym_480.bin"`) from the `(locale, file)` the
picker asks for and looks that up. The app must stage endonym blobs under that same
composite key. This mirrors the screens repo's web_runner, which hit the identical
collision and added a locale-keyed `ss_stage_endonym` map alongside its
filename-keyed pack map.

## Non-obvious constraint 2 — GC-safety of the staging dict

A Python dict referenced only from a C `static` is **not** a GC root in
MicroPython and can be collected out from under you. `load_locale` sidesteps this
by building its `mp_pack_ctx_t` on the C stack during the call and taking the dict
as an argument (kept alive by the caller's Python frame); the provider is used
only synchronously within that call.

`locale_picker_screen(cfg, endonym_images)` uses the **same pattern on purpose**:

1. `endonym_images` is a call **argument** → GC-alive for the whole call.
2. The binding points the picker's provider at a stack `mp_pack_ctx_t`.
3. `run_screen(locale_picker_screen, …)` builds the screen **synchronously** — the
   picker fetches and *copies* every endonym blob into an `lv_draw_buf` during the
   build (`parse_ssa8` in `locale_picker.cpp`: "copies; provider bytes may now go
   stale").
4. The binding then clears the provider (`dm_set_endonym_image_provider(NULL,
   NULL)`) so the C side never holds a dangling pointer to the now-dead stack ctx
   or the dict.

Because of step 3, the dict need only survive the call, and the draw/delete
callbacks that keep the picker on screen use the copied `lv_draw_buf`, never the
provider — clearing it after the build is safe. **Do not** try to stash the dict in
a static and set the provider from a separate binding: that reintroduces the
GC-root problem the argument-passing avoids.

## Non-obvious constraint 3 — `locale_picker.h` / `locale_fonts.h` can't be included in the binding

MicroPython's QSTR scan compiles `modseedsigner_bindings.c` with only the include
dirs listed in `bindings/micropython.cmake` — **not** the transitive ESP-IDF
component include paths, so `lvgl.h` is absent there. `locale_picker.h` includes
`lvgl.h`, and `locale_fonts.h`/`supported_locales_json()` are C++ (`std::string`).
Both would fail the QSTR scan.

That is why every call goes through a plain-C `dm_*` shim in
`display_manager.cpp` (which *does* link LVGL and compile as C++): the binding sees
only lvgl-free, C-ABI signatures. `dm_supported_locales_json()` bridges the C++
`std::string` return to a `const char*` (static buffer, valid until the next
call — same lifetime contract as `ss_locale_pack_files()`), which the binding
copies into a Python `str`. This is the same split already used for `dm_mem_stats`.

## Locking

`dm_supported_locales_json` / `dm_register_pack_manifest` /
`dm_clear_pack_manifests` touch the render layer's compiled+runtime locale table,
which `ss_load_locale` (under the LVGL-port lock) also reads. All three take the
lock too, so a table read/write is serialized against a concurrent load — the same
discipline as `dm_load_locale`. `dm_set_endonym_image_provider` only stores two
pointers the picker reads later (during `locale_picker_screen`, itself under the
lock via `run_screen`), so it needs no lock — matching `dm_set_cache_psram`.

## `list_available_locales()` is the font manifest, not a display table

It returns `supported_locales_json()` verbatim: profile + the pack locales
(compiled-in fonts ∪ runtime SD packs), each with `source_family`/`chain`/`fonts`.
It intentionally does **not** carry the native endonym string, the English display
name, or a live-text-vs-image flag — `ss_register_pack_manifest` discards `endonym`
(the `LocaleFontEntry` struct has no such field), and baked-floor Latin locales
(English, Español, …) have no font entry at all, so they never appear here. The
`seedsigner` app owns all display metadata: it adds English + Latin locales from
its own translation catalog and decides live-vs-image per language via the
baked-floor glyph-coverage test (see the screens knowledge doc). The builder's job
is only "which locales can the firmware actually render right now."

## Submodule pin is on an unmerged feature branch (interim)

`deps/seedsigner-lvgl-screens` is pinned to
`feat/locale-picker-and-pack-discovery` @ `571abc8`, not a merged upstream commit,
because the screens repo hasn't merged the picker work yet. That tip is screens
`main` (`314b392`) + the 3 picker/discovery commits, so it also pulls in the
already-merged `psbt_overview_screen`/`btc_amount` (compiled into `__idf_seedsigner`,
currently unbound — harmless). **Re-pin to the upstream merge commit when the
screens PR lands.**
