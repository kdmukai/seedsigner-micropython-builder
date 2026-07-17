# CXX exceptions disabled → any screen `throw` aborts the chip

**Status:** ✅ fixed on all release boards (P4-43 gap closed 2026-07-16; P4-35 already had it).

## Symptom

Navigating to a screen that hits a validation/render error panics the board instead of
showing a recoverable error. On the P4-43 (MIPI-DSI) this looks like a **light-blue screen
with garbled serial output** — the partially-composited LVGL background left on the panel,
plus the panic register/backtrace dump. **No coredump is written** that matches the running
app (the abort cascade happens inside the C++ exception stubs, and the stale dump from a prior
firmware fails the app-SHA check), so `esp_coredump info_corefile` reports
`coredump SHA != app SHA` — a tell that this is the failure mode rather than a clean panic.

First observed via the SeedQR **zoomed transcribe** view: the app reached
`seed_transcribe_zoomed_qr_screen` without the required `exit_text` cfg key, the screen did
`throw std::runtime_error("exit_text is required…")`, and the chip aborted.

## Root cause

The `seedsigner-lvgl-screens` library reports config/render errors by **throwing** C++
exceptions; `run_screen()` (`ports/esp32/display_manager/display_manager.cpp`) wraps the screen
builder in `try { cb(ctx); } catch (const std::exception& e) { err = e.what(); }` and
`run_cfg_screen()` re-raises `err` as a **Python `ValueError`**. This whole contract requires
C++ exceptions to be **enabled**.

When `CONFIG_COMPILER_CXX_EXCEPTIONS` is **off** (the ESP-IDF default), the linker replaces the
Itanium C++ ABI entry points with abort stubs (`components/cxx/cxx_exception_stubs.cpp`):

```cpp
extern "C" void __wrap___cxa_allocate_exception(void) { abort(); }
extern "C" void __wrap___cxa_throw(void)              { abort(); }
```

So the **first** `throw` anywhere in the screen code calls `abort()` → `esp_system_abort` →
`panic_abort`, before `run_screen`'s `catch` can ever run. The decoded backtrace is a giveaway:

```
abort() was called at PC 0x…  →  __wrap___cxa_allocate_exception (cxx_exception_stubs.cpp)
0x…: panic_abort            (esp_system/panic.c)
0x…: esp_system_abort       (esp_system/port/esp_system_chip.c)
```

(the "repeating frame" in the raw backtrace is just the panic/abort call chain, not real
recursion.) `run_screen`'s `catch` block is dead code with exceptions off — it can't compile a
working handler, so a throw is unconditionally fatal.

Why it stays hidden: it only bites when a screen **actually throws**, which only happens on a
missing/invalid required cfg key. Every screen driven with a complete cfg renders fine, so the
landmine sits latent until the first app path that omits a required key.

## Fix

Enable C++ exceptions in the board's `sdkconfig.board`:

```
CONFIG_COMPILER_CXX_EXCEPTIONS=y
```

Now a `throw` allocates a normal exception, unwinds to `run_screen`'s `catch`, and surfaces as a
Python `ValueError` naming the failing screen + key — recoverable, and it names the bug instead
of bricking the session. Verified on-device: the same `seed_transcribe_zoomed_qr_screen` call
that aborted now raises `ValueError('seed_transcribe_zoomed_qr_screen: exit_text is required…')`
and the board keeps running.

### Notes / gotchas

- **Orthogonal to `COMPILER_STACK_CHECK`.** The stack-protector (`-fstack-protector*`) is a
  *separate* setting that must stay **off** — its canary epilogues break MicroPython's NLR
  (setjmp/longjmp) unwinding. Enabling exceptions does not touch that; the P4-35 runs both
  states (exceptions on, stack-check off) fine.
- **Emergency pool** (`CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE`) is left at the default `0`,
  matching the P4-35: exceptions allocate from the normal heap, which is available during
  routine validation throws. (If the heap were exhausted at throw time, allocation would still
  abort — but that's an OOM condition, not this failure class.)
- Enabling exceptions recompiles **every C++ TU** (the flag changes codegen: unwind tables +
  `-fexceptions`) and adds ~20 KB to the image. `CONFIG_ESP_SYSTEM_USE_EH_FRAME=y` is already
  set for panic backtraces, so the `.eh_frame` tables are largely already present.
- This was a **per-board gap**, not a global regression: the P4-35 board had
  `CONFIG_COMPILER_CXX_EXCEPTIONS=y`; the P4-43 board's `sdkconfig.board` simply never included
  it, so it inherited the IDF default (off).

## The separate app-side bug this exposed

The trigger was a genuine app bug: `SeedTranscribeSeedQRZoomedInView` (seedsigner
`views/seed_views.py`) called `seed_transcribe_zoomed_qr_screen` **without** the `exit_text`
cfg key, which the screen requires in every input mode (touch builds don't render it, but the
contract is uniform). Fixed app-side by passing `exit_text=_("click to exit")` (the existing,
already-translated msgid used by the PIL `SeedTranscribeSeedQRZoomedInScreen`). The
firmware-config fix and the app fix are independent: exceptions-on prevents the *hard crash*
(→ recoverable `ValueError`); the app fix makes the flow *actually complete*.
