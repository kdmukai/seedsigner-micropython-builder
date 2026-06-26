# Large MicroPython scripts hang the COMPILER (O(n²) constant dedup) → watchdog reboot

## Symptom
Running a large `.py` on-device (e.g. `mpremote run big_script.py`) triggers a task watchdog
panic **before any of the script's own output appears**:

```
E task_wdt: Task watchdog got triggered ... IDLE1 (CPU 1) did not reset ...
E task_wdt: CPU 1: mp_task
```

The decoded `mp_task` backtrace lands entirely in the **compiler**, not the runtime:

```
mp_task → do_reader_stdin → parse_compile_execute → mp_compile_to_raw_code
        → compile_scope → compile_atom_bracket → compile_dictorsetmaker_item
        → emit_write_bytecode_byte_obj → mp_emit_common_use_const_obj
        → strictly_equal → mp_obj_str_equal
```

## Root cause
While emitting bytecode, MicroPython **de-duplicates constants** so identical literals share one
object. `mp_emit_common_use_const_obj` (py/emitcommon.c) does this with a **linear scan + equality
compare against every constant seen so far** — i.e. **O(n²)** in the number of distinct constants,
and each compare is a full `mp_obj_str_equal` (byte-compare of the whole string).

A script with thousands of long string literals (our case: ~3,300 translated UI strings, many
multi-byte CJK, embedded as list/dict literals) costs ~n²/2 ≈ millions of full-string compares. That
runs synchronously on `mp_task`, never yields, and the **task watchdog fires while still compiling**.
The script's code never executes. It is NOT an out-of-memory crash (no `LV_ASSERT_MALLOC`, no
`MemoryError`) — it's pure compile-time CPU.

## Fix: don't emit thousands of literals
Store the bulk data as **a few big joined strings** and rebuild the structures at runtime with
`.split()`. The compiler then dedups ~2 constants instead of thousands (instant), and `split()` is a
linear runtime operation with no dedup pass:

```python
# BAD: ~3300 string constants -> O(n^2) compile -> WDT
CORPORA = {"zh": ["设置", "单签", ...350...], "ja": [...], ...}

# GOOD: ~2 string constants -> instant compile; lists rebuilt at runtime
LOCS = "zh ja ...".split()
_C = "设置\x1e单签\x1e...\x00...\x1e..."   # \x1e between items, \x00 between groups (control
CORPORA = {LOCS[i]: b.split("\x1e")        #  chars guaranteed absent from UI text)
           for i, b in enumerate(_C.split("\x00"))}
_C = None; gc.collect()
```

Pick separators that cannot occur in the data (we used `\x1e`/`\x00`). `json.dumps(s, ensure_ascii=
False)` emits the blob as one valid source string literal (control chars become `\u00xx`, text stays
raw). Verify the runtime split reconstructs the original exactly.

Alternative if the data is huge: ship it as a separate JSON/binary file and `json.loads(open(...))`
at runtime — runtime parsing also skips the compiler's const-dedup. Self-contained joined-strings are
simpler when you want a single `mpremote run`.

## Where this bit us
`tools/stress_locale_churn.py` — the maximal i18n font-memory stress test embeds the full per-locale
translation corpora (~3,300 strings across 11 locales). The first version used list/dict literals and
WDT-rebooted in the compiler at ~49 s; re-encoding as two joined strings fixed it. See
`docs/approach-a-cache-psram-design.md`.

## Rule of thumb
Keep on-device scripts to a few hundred distinct literals. Past ~1,000 long string literals the
compile cost is already noticeable; a few thousand will trip the watchdog. Bulk data → joined strings
or an external file, never thousands of literals.
