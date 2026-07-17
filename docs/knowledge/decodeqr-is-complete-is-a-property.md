# DecodeQR.is_complete is a @property, not a method — calling it crashes

**Applies to:** any code driving the SeedSigner app's `DecodeQR`
(`seedsigner/models/decode_qr.py`) on MicroPython or CPython.

## Symptom

A scan/UR loop that feeds `decoder.add_data(payload)` and then checks
`decoder.is_complete()` **never registers a completion** and, depending on
error handling, either crashes with
`TypeError: 'bool' object is not callable` or silently makes no progress. The
UR fountain assembles fine (the app scans the same PSBT in 15–20 s) but the
harness sees zero completions.

## Root cause

`DecodeQR` mixes two API styles and it is easy to conflate them:

- **Properties** (no parentheses): `is_complete`, `is_psbt`, `is_seed`,
  `is_invalid`, `is_address`, `is_sign_message`, `is_wallet_descriptor`, …
  ```python
  @property
  def is_complete(self) -> bool:
      return self.complete
  ```
- **Methods** (call with parentheses): `add_data(...)`,
  `get_percent_complete(...)`, `get_psbt()`, `get_seed_phrase()`, …

`decoder.is_complete` evaluates to a `bool`. Writing `decoder.is_complete()`
evaluates the property to a bool and then **calls the bool** →
`TypeError: 'bool' object is not callable`. If that `TypeError` is caught and
swallowed (e.g. `except Exception: pass`), the completion branch simply never
runs — no crash, no completion, just silent non-progress.

The app itself uses the property form correctly, e.g. `scan_views.py`:
```python
if self.decoder.is_complete:      # property, no ()
```

## Correct usage

Two safe options:

1. Read the property: `if decoder.is_complete: ...`
2. Read `add_data`'s return status (preferred in a feed loop, since it also
   tells you *this* frame's effect): `add_data` returns a `DecodeQRStatus`
   integer; `DecodeQRStatus.COMPLETE == 3`.
   ```python
   if decoder.add_data(payload) == 3:   # COMPLETE
       ...
   ```

`get_percent_complete()` IS a method (it takes `weight_mixed_frames`), so it
*does* need parentheses — the inconsistency is the trap.

Hit 2026-07-16 in the instrumentation benchmark harness: `_drain_new_parts`
called `is_complete()`, so K1/K2 benchmark slots never completed and the
uncaught `TypeError` killed the whole run (blank screen). The native cUR
(`uUR`) decoder was blameless. Fix: switch to the `add_data == 3` return-value
check.
