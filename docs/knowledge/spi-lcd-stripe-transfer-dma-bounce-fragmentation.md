# SPI LCD stripe transfers: the odd 32767-byte cap → per-blit DMA bounce → fragmentation freeze

## Symptom

On the ESP32-P4 ST7796 SPI board (WAVESHARE_ESP32_P4_WIFI6_TOUCH_LCD_35), the
camera preview would run fine for the first couple of scan cycles, then the panel
froze mid-scan with a flood of:

```
E board: DIAG blit ret=ESP_ERR_NO_MEM 80,0..400,60
```

`esp_lcd_panel_draw_bitmap()` returned `ESP_ERR_NO_MEM` on every square-region
stripe blit. Critically, at the point of failure **internal RAM had plenty of
total free** (~136 KB) but the **largest contiguous DMA-capable block had
collapsed** (65536 → 32768). So it was fragmentation, not exhaustion.

## Root cause

The square camera image is blitted to the panel in horizontal stripes via
`esp_lcd_panel_draw_bitmap()` from a byte-swapped internal DMA buffer
(`ctx->dma_buf`, `heap_caps_aligned_alloc(64, …, MALLOC_CAP_INTERNAL |
MALLOC_CAP_DMA)`). A 60-line stripe is 320 × 60 × 2 = **38400 bytes**.

The ESP32-P4 (and S3) GPSPI length register `SPI_MS_DATA_BITLEN` is **18 bits**:

```
#define SPI_MS_DATA_BITLEN  0x0003FFFF   // 262143 bits
```

so `spi_bus_get_max_transaction_len()` returns
`MIN(max_transfer_sz, 262143/8) = 32767 bytes` — an **odd** value. `esp_lcd`'s
`panel_io_spi_tx_color()` splits any color transfer larger than that at byte
offset **32767**, which is *not* cache-line (64-byte) aligned.

In `spi_master.c::setup_priv_desc()` the tx path bounces when the buffer is
non-DMA-capable **or** unaligned:

```c
bool tx_unaligned = ((((uint32_t)send_ptr) | tx_byte_len) & (alignment - 1)); // alignment = 64 on P4
if (!esp_ptr_dma_capable(send_ptr) || tx_unaligned) {
    uint32_t *temp = heap_caps_aligned_alloc(alignment, tx_byte_len, MALLOC_CAP_DMA); // per transfer!
    ...
}
```

The split at the odd 32767 boundary makes the first chunk 32767 bytes long
(low 6 bits = 63 ≠ 0) → `tx_unaligned` true → the SPI master allocates a fresh
**~32 KB bounce buffer from the DMA-capable heap on every single blit**, then
frees it. That alloc/free churn of a large, exact-sized block progressively
**fragments the internal DMA pool**. Once the largest contiguous DMA block drops
below the bounce size, the alloc fails → `NO_MEM` → the panel stops updating.

The stripe buffer itself was DMA-capable and 64-aligned — so nothing looked
wrong at the whole-transfer level. The unalignment is introduced *internally* by
esp_lcd's chunk split at the hardware's odd max-transaction size. That is why it
was non-obvious: the failing buffer passes every check you'd inspect directly.

## Fix

Cap the stripe height so each blit is a **single sub-32767-byte chunk** (no
split). Because `row_bytes = width * 2 = 640` is a multiple of 64 for the 320-wide
camera square, any `640 × lines` transfer stays 64-aligned, so the blit becomes
**zero-copy**: no per-blit bounce, no DMA-pool churn/fragmentation, and it avoids
the bounce memcpy (slightly faster). See `SPI_MAX_SINGLE_XFER_BYTES` in
`ports/esp32/board_common/src/board_pipeline_display_lvgl.c`.

The gutter blit buffer (`s_gutter_dma` in `board_init.c`) was also switched from
`heap_caps_malloc` to `heap_caps_aligned_alloc(64, …)` for the same reason.

## Validation

On device: internal DMA `largest` block stayed stable (~45–73 KB, coalescing
upward) across six consecutive scan cycles with zero `NO_MEM`; user confirmed
multiple animated + static QR scans with no glitches, no frame-rate change, no
freeze.

## Related

This is a **separate** failure mode from the cUR fountain decoder's internal-RAM
fragmentation (fixed by routing cUR allocations to PSRAM — odudex/cUR#9). Both
had to be fixed for reliable scanning: the cUR fix stopped internal RAM from
being consumed/fragmented by the decoder, and this fix stopped the SPI blit path
from churning the DMA pool. GPSPI cannot DMA from PSRAM on these parts, so the
stripe buffer must stay in internal DMA RAM — which is exactly why keeping that
pool unfragmented matters.
