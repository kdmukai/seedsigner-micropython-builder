# Nano-newlib printf has no 64-bit support — `%lld` in C snprintf crashes

**Applies to:** all ESP32 firmware built by this repo. The board sdkconfigs set
`CONFIG_NEWLIB_NANO_FORMAT=y` (nano formatted-I/O — smaller code, no 64-bit or
C99 specifiers).

## Symptom

A task crashes inside `_printf_i` (newlib integer formatting) with the PC
jumping to a garbage/ROM address, while the task's stack shows **plenty of
free space** (i.e. it is NOT a stack overflow). Coredump `info_corefile`
points the crashed frame at `_printf_i+NNN` with an unwind that dies at a
bogus address.

Real example (2026-07-16, instrumentation build): the `qr_decode` task
crashed the instant a scan slot started capturing, `_printf_i+524`, stack
4960 used / 11276 free. The trigger was a per-dispatch CSV row formatted with
`snprintf("%u,%lld,...", ..., (long long)ts_us, ...)`.

## Root cause

Nano `vfprintf` does **not** implement the `ll` (64-bit) length modifier. When
it parses `%lld` it consumes the modifier but then reads a 32-bit `int` from
the `va_list` instead of a 64-bit `long long`. That leaves the vararg cursor
**4 bytes short**, so every subsequent conversion reads shifted garbage — a
later `%s`/`%d` dereferences a bad pointer and faults *inside printf*, which is
why the backtrace looks like "printf crashed" rather than pointing at the
caller.

Floating point (`%f`, `%.1f`) is a separate story: the **P4 ROM** nano
formatter *does* support `%f`, so those work in `snprintf` here (verified: the
2 s stats line formats floats every interval without crashing). Only the
64-bit integer specifiers are the trap. `ESP_LOGx` may appear to handle
`%lld` because it can route through a different vprintf — do not infer from a
working `ESP_LOGI("%lld")` that `snprintf("%lld")` is safe.

## Fix / rule

**Never pass a 64-bit value to a C `printf`-family call in this firmware.** For
`int64_t` (e.g. `esp_timer_get_time()` timestamps) format the number yourself
into a small `char[24]` and emit it with `%s`:

```c
static void fmt_i64(char *out, size_t n, int64_t v) {
    char tmp[24]; int i = 0;
    uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    if (!u) tmp[i++] = '0';
    while (u) { tmp[i++] = (char)('0' + (int)(u % 10)); u /= 10; }
    size_t o = 0;
    if (v < 0 && o + 1 < n) out[o++] = '-';
    while (i > 0 && o + 1 < n) out[o++] = tmp[--i];
    out[o] = '\0';
}
```

The crash is silent at compile time: gcc's `-Wformat` treats `%lld` +
`(long long)` as CORRECT (it is, per C — the bug is nano's runtime, not the
types), so `-Werror=format` does **not** catch it. Only a device run (or
knowing this rule) surfaces it.

Passing values to the MicroPython layer is fine: `mp_obj_new_int_from_ll()`
uses MicroPython's own bignum formatter, not newlib.
