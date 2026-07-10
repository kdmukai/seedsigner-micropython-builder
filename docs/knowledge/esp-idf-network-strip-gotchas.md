# Stripping networking from the ESP32 firmware: every trap, and why the first attempt failed

**Context (2026-07-10):** P1 of the network-strip run — remove WiFi/BT/LWIP/esp_netif and every
HTTP/TLS/OTA/provisioning stack from the P4 firmware so the air gap is a *compile-time verifiable*
property. A dormant `MICROPY_DISABLE_NETWORK` mechanism existed in the MicroPython patch but had
never worked. It turned out to be *two* silent no-ops plus three transitive re-pull traps. All are
non-obvious; each cost the original attempt.

## The two silent no-ops in the original machinery

### 1. ESP-IDF evaluates component CMakeLists in script mode — `-D` cache args are invisible there

IDF's build system runs an **early component expansion** pass that evaluates each component's
CMakeLists in **CMake script mode** to collect `idf_component_register(REQUIRES ...)` metadata.
That metadata builds the component dependency graph — it decides *which components enter the
build at all*. Script mode has **no CMake cache**, so a `-DMICROPY_DISABLE_NETWORK=ON` from the
command line is undefined in that pass:

- Early pass: flag reads OFF → `esp32_common.cmake` reports the **networked** `IDF_COMPONENTS`
  list → lwip/esp_wifi/bt/etc. enter the graph and **compile**.
- Real pass: flag reads ON → main links against the **minimal** list → the network archives
  contribute **zero bytes** to the image.

The result *looks* stripped in the final `message(STATUS)` output and links clean, but ~20
network components still compile and sit on the linker command line. Diagnosis fingerprint: the
same `message(STATUS)` line printing **twice per configure with different values**.

**Fix:** detect the flag from the **environment variable** too (`$ENV{MICROPY_DISABLE_NETWORK}`),
which script mode *can* see. `build_firmware.sh` exports it alongside the `-D`. (Same quirk
family as the `screen_sources.cmake` CONFIGURE_DEPENDS script-mode guard in seedsigner-lvgl-screens.)

### 2. `EXCLUDE_COMPONENTS` set inside a component CMakeLists is silently ignored

IDF honors `EXCLUDE_COMPONENTS` **only at project level, set before
`include($ENV{IDF_PATH}/tools/cmake/project.cmake)`** (i.e. in `ports/esp32/CMakeLists.txt`).
The original machinery set it in `main/CMakeLists.txt` — a component CMakeLists, processed
*after* component discovery. No error, no warning, no effect. It printed a convincing
`MAIN_EXCLUDE_COMPONENTS=...` STATUS line for months while doing nothing.

**Fix:** the exclusion list lives in `ports/esp32/CMakeLists.txt` (already part of the
MicroPython patch), gated on the same cache-or-env detection.

Also note: **without a project-level `COMPONENTS`/`EXCLUDE_COMPONENTS` narrowing, IDF builds every
discovered component** — even ones nothing requires (mqtt, esp-tls, esp_hid… all compiled in the
baseline with zero requirers). Exclusion at discovery is the only way to keep them out.

## The transitive re-pull traps

1. **`espressif/mdns` in `main/idf_component.yml`** hard-requires `lwip` + `esp_netif`. As long as
   it's in the manifest, the component manager pulls the TCP-IP stack back in no matter what you
   exclude. Upstream MicroPython also explicitly declares **`espressif/esp_hosted` +
   `espressif/esp_wifi_remote` for `target == esp32p4`** — the P4→C6 radio transport (plus their
   own deps: eppp_link, esp_serial_slave_link, protocomm, protobuf-c...). All three removed from
   the manifest for offline firmware.
2. **tinyusb requires `esp_netif`** — for a single header (`netif/ethernet.h` in
   `rndis_reports.c`), because it unconditionally compiles its USB networking class sources
   (ECM/RNDIS/NCM), even when the tusb config never enables them (zero bytes placed). Since we
   keep tinyusb (REPL/deploy console), `esp_netif`→`esp_netif_stack`→`lwip` stayed in the compile
   graph. **Fix:** `deps/micropython/mods/component_patches/tinyusb-no-usb-net-class.patch`
   (applied by `apply_component_patches.sh`, same mechanism as the LVGL PSRAM patches) removes the
   NET class sources and the esp_netif requirement — USB networking is not even compiled.
3. **Excluding a component that a kept component still REQUIRES fails configure — order matters on
   fresh trees.** The `submodules` reconfigure is what *fetches* managed components; the tinyusb
   patch can only apply after the fetch. So `build_firmware.sh` runs: fetch pass **unstripped** →
   `apply_component_patches.sh` → real build **stripped**. Don't "simplify" this into passing the
   strip flags to both passes; a fresh clone/CI build will die in the fetch pass resolving
   tinyusb's esp_netif requirement.

## Things that DON'T work (so you don't retry them)

- **`CONFIG_ESP_NETIF_TCPIP_LWIP=n` in sdkconfig fragments**: it's a kconfig *choice* member;
  kconfgen re-selects it while esp_netif is in the build. Belt-and-suspenders `=n` lines are still
  worth keeping for plain bools (ETH_ENABLED etc.), but the real guarantee is component absence.
- **Relying on `MICROPY_PY_NETWORK=0` etc. (the C-level API config)**: that only removes the
  *Python-facing* API. The IDF stack still compiles, links, and (some of it) initializes at boot.
  This was exactly the pre-P1 state: Python API off, `esp_wifi`/`lwip`/`esp_netif` in the image.

## One MicroPython source fix

`ports/esp32/main.c` unconditionally `#include "modnetwork.h"`, which drags in
`esp_wifi_types.h`/`esp_netif.h`. With the components truly absent this is a fatal include error.
main.c only needs it for `socket_events_deinit()`, already guarded — so the include is now wrapped
in `#if MICROPY_PY_NETWORK || MICROPY_PY_SOCKET_EVENTS`.

## How to verify a strip (the audit that actually proves it)

```sh
# 1. Components that entered the build at all:
grep -o -E "LOAD esp-idf/[a-z_0-9-]+/lib[a-zA-Z_0-9-]+\.a" build/<BOARD>/micropython.map | sort -u
# 2. Bytes actually placed in the image from network archives (must be 0 matches):
grep -c -E "lib(lwip|esp_netif|esp_wifi|esp_eth|bt|wpa_supplicant|esp-tls|esp_http_client|mqtt|protocomm)\.a\(" \
    build/<BOARD>/micropython.map
# 3. Who still requires what (find re-pulls):
python3 - <<'EOF'
import json; d=json.load(open('build/<BOARD>/project_description.json'))
c=d['build_component_info']
for n,i in c.items():
    for r in i.get('reqs',[])+i.get('priv_reqs',[]):
        if r in ('lwip','esp_netif','esp_wifi','bt'): print(f'{r} <- {n}')
EOF
# 4. Runtime: boot log must show no wifi/netif/lwip/BT/SDIO-slave init lines.
```

`LOAD` lines in the map = archive on the linker command line (compiled). `archive.a(member.obj)`
entries = code actually placed. The security claim needs #2 and #4; #1/#3 are the cleanliness
audit that keeps the graph honest.
