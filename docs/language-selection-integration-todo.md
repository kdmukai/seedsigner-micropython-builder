# TODO: wire the language-selection picker (ESP32 host seam)

_Handoff from the `seedsigner-lvgl-screens` session that built the picker + runtime
pack discovery (2026-07-03). This is the ESP32 half; do it in its own session._

## Sequencing gate (blocked until)

The shared screen changes must land first: **the `seedsigner-lvgl-screens` change
merges upstream (kdmukai/seedsigner-lvgl-screens) and this repo bumps its
`deps/seedsigner-lvgl-screens` submodule pin** to include it. Order: screens (done)
→ this repo → the `seedsigner` app.

> **Interim pin (2026-07-05):** the screens change is NOT yet merged to
> `kdmukai/seedsigner-lvgl-screens` main (the screens repo is on other
> priorities). So this repo's `deps/seedsigner-lvgl-screens` submodule is pinned
> **directly to the committed feature branch tip** —
> `feat/locale-picker-and-pack-discovery` @ `571abc8` (= screens `main` `314b392`
> + the 3 picker/discovery commits; it also carries the already-merged
> `psbt_overview_screen`/`btc_amount` work). **Re-pin to the upstream merge commit
> once the screens PR lands.**

## Builder-side native surface — IMPLEMENTED 2026-07-05

The ESP32 native module (`seedsigner_lvgl_screens`) now exposes everything the
`seedsigner` app needs to drive the picker + runtime pack discovery. **What is
done here (this repo) vs. what remains app-side (the separate `seedsigner`
session) is called out per task below.** New bindings:

- **`locale_picker_screen(cfg=None, endonym_images=None)`** — runs the new screen.
  `cfg` is the standard dict→JSON config; `endonym_images` is an optional dict
  **`{"<locale>/<file>": bytes}`** holding each image row's pre-rendered endonym
  blob (e.g. `{"hi/endonym_480.bin": b"SSA8…"}`). The binding wires that dict to
  the picker's image provider for the (synchronous) build, then clears the
  provider — the picker copies each blob it keeps, so the dict need only live for
  the call (passed as an argument = GC-alive). Selection returns on the shared
  poll queue as a `button_selected` event whose index is the row position.
- **`register_pack_manifest(manifest) -> bool`** — registers a runtime SD pack
  from its `manifest.json` bytes; fails closed (returns `False`, never raises).
- **`clear_pack_manifests()`** — drop all runtime packs before a rescan.
- **`list_available_locales() -> str`** — JSON (`supported_locales_json()` shape:
  `{"profile":…,"locales":[{"locale","source_family","chain","fonts":[…]}…]}`) of
  every locale renderable as a pack (compiled-in fonts ∪ runtime SD packs) for the
  active display profile. **Baked-floor Latin locales (English, …) are NOT in this
  list** (they need no pack) — the app adds them from its own translation catalog,
  and the app supplies each locale's English display name + native endonym +
  live-text-vs-image decision (all app-owned; the C layer does not retain them).

Under the hood these route through `dm_*` wrappers in
`ports/esp32/display_manager/display_manager.cpp` that take the LVGL-port lock
(same discipline as `dm_load_locale`). The endonym provider
(`mp_endonym_provider` in `bindings/modseedsigner_bindings.c`) keys by
`"<locale>/<file>"` — NOT filename alone like `mp_pack_provider` — because the
picker fetches many locales' images at once and they all share the name
`endonym_<h>.bin`. See `docs/knowledge/locale-picker-endonym-provider-micropython.md`.

## What the screens layer now gives you

- **`locale_picker_screen(cfg_json)`** — a new screen. cfg:
  ```json
  { "top_nav": {"title": "Language", "show_back_button": true},
    "active_locale": "<code>",
    "rows": [ {"locale":"en","english":"English","native":"English"},        // live
              {"locale":"es","english":"Spanish","native":"Español"},        // live
              {"locale":"hi","english":"Hindi","native":"हिन्दी","image":"endonym_320.bin"} ] }
  ```
  Each row is a SINGLE line `"English | native"`: English name live, native either
  live text (Latin the baked floor covers) or — when the row carries `"image"` (a
  filename or `true` → `endonym_<active-profile-height>.bin`) — a pre-rendered image
  after the "|". **Live-vs-image = baked-floor glyph coverage of the NATIVE name, NOT
  "ships a pack"** (floor = ASCII + Latin-1 + Latin Ext-A + General Punctuation; so
  Español/Čeština are live but Vietnamese "Tiếng Việt" is an image, as are all
  non-Latin scripts). Selection fires `seedsigner_lvgl_on_button_selected(row_index,
  ...)` — map index → the locale you placed there.
- **`locale_picker_set_image_provider(ss_pack_provider_t, void*)`**
  (`locale_picker.h`) — the byte provider the picker uses to fetch endonym images.
  **Same seam/signature as `ss_load_locale`'s provider.** Your `mp_pack_provider`
  (dict-backed, in `bindings/modseedsigner_bindings.c`) serves `endonym_<h>.bin`
  the same way it serves `.ttf`/`runs.bin` — just add the endonym blobs to the
  staging dict and point the picker at the same provider.
- **`ss_register_pack_manifest(json, len)` / `ss_clear_pack_manifests()`**
  (`locale_loader.h`) — register a runtime-discovered pack from its own
  `manifest.json`, so `ss_load_locale()` works for a not-compiled-in locale (the
  "copy a pack onto the SD card, no firmware rebuild" path). Fails closed.
- Each pack's **`manifest.json` is self-describing** now: `locale`, `source_family`,
  `chain`, `rtl`, `shaping`, `script`, `unicode_range`, `endonym`,
  `endonym_images: {"240": {"file": …}, "320": …, "480": …}`, plus font/runs files.
  Endonym images are A8 "SSA8" blobs (see the `locale-picker-and-endonym-images.md`
  knowledge doc in the screens repo).

## Tasks

1. **Discover packs on the SD card.** — **APP-side (separate `seedsigner`
   session).** The native `register_pack_manifest(bytes)` binding is now provided;
   the app does the SD I/O. Your C loader can't open the SD directly (the
   fatfs/oofatfs link clash — see `micropython-fatfs-vs-esp-idf-fatfs-collision.md`),
   so Python lists `<packs-partition>/lang-packs/` via `machine.SDCard`+VFS, reads
   each `manifest.json`, and passes the bytes to `register_pack_manifest`.
   **Defensive discovery** (user-writable cross-platform FAT/exFAT volume): skip
   desktop-OS junk (`.DS_Store`, `._*` AppleDouble, `.Spotlight-V100`, `.Trashes`,
   Windows `System Volume Information`) and half-copied dirs; a bad manifest is
   simply omitted (`register_pack_manifest` returns `False`) — never crash.
2. **Stage endonym images + set the picker provider.** — **Native seam DONE;
   staging is APP-side.** Do NOT call `locale_picker_set_image_provider` from
   Python (its header pulls `lvgl.h`, kept out of the QSTR-scan set). Instead the
   app reads each pack's `endonym_<active-height>.bin` into a dict keyed
   **`"<locale>/endonym_<h>.bin"`** and passes that dict as the
   `endonym_images` argument to `locale_picker_screen(cfg, endonym_images)`; the
   binding sets the provider (keyed by `(locale, file)`) for the build and tears
   it down after. N small A8 blobs are held only while the screen builds.
3. **PSRAM for the endonym A8 buffers.** — **Code-wise satisfied; needs on-device
   measurement.** The picker parses each blob into an A8 `lv_draw_buf`
   (`lv_draw_buf_create(..., LV_COLOR_FORMAT_A8, 0)`), which uses the **DEFAULT**
   draw-buf handlers. Those are already routed to PSRAM by
   `route_draw_buffers_to_psram` (`display_manager.cpp`; the ur-mask OOM fix), so
   endonym buffers land in PSRAM automatically — no new code needed. **Still to do:
   confirm + measure** on P4/S3 once the app can drive the picker (internal pool
   must stay clear; the whole point of images-over-fonts is to avoid
   glyph-cache-node pressure). `mem_stats()` reports `spiram_*` / `lvgl_*`.
4. **Expose to Python** — **DONE (this repo).** `list_available_locales()` returns
   the compiled ∪ runtime pack-locale manifest for the active profile (see the
   native-surface section above); `locale_picker_screen(cfg, endonym_images)` runs
   the screen. **The C layer does NOT carry the endonym string / English name /
   has-image flag** (`supported_locales_json()` is the font manifest, not a display
   table), so the `seedsigner` app owns those: it decorates each locale from its
   own translation catalog + the baked-floor glyph-coverage test, then builds the
   picker cfg. English + Latin locales (absent from `list_available_locales()`) are
   added by the app.

## Partition topology (target)

Future stage-1 bootloader: the full MicroPython firmware ships on the SD card,
ideally on a partition **hidden** from desktop OSes; language packs on a **separate,
user-writable, cross-platform-visible (FAT/exFAT) partition** so users can drag
packs onboard. Point discovery + the provider at the packs-partition mount.

## Blocker for "done": on-device translated TEXT (Layer 1)

Selecting a language on ESP32 currently switches the SCRIPT/fonts but the UI text
stays English — `seedsigner/compat/l10n.py` is an identity passthrough on
MicroPython (no gettext). Full "language selection working" needs the Layer-1 `.mo`
text layer built + deployed. Spec:
`seedsigner-lvgl-screens/docs/i18n-ngettext-micropython-spec.md` (Deliverables A+B);
device/deploy half: `micropython-i18n-mo-catalog-deploy.md` (this repo). This is a
separate required workstream — see the `seedsigner` repo's todo.
