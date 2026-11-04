# Sound Blaster DMA ownership on Wyse

The guest CPU and emulated 8237 controller run on core 0. Core 1 consumes the
Sound Blaster PCM ring and mixes the hardware audio output. On Wyse, core 0
must also own the Sound Blaster's DMA request (DREQ).

Previously, every output sample could reassert DREQ after testing
`dma_running`. Core 0 could finish a single-cycle block and clear DREQ between
that test and the assertion. The consumer then resurrected the request. The
transfer callback accepted it even though the DSP had stopped, allowing PCM
to be copied before the guest armed the next block.

`control()` now keeps DREQ asserted on core 0 for the duration of a transfer.
Consuming the ring frees space for the next `pc_step()` refill; it does not
need to reassert DREQ from core 1. The transfer callback also rejects an
inactive DSP's stale request. Other targets retain their existing request
handling.

## Validation

`tools/tests/sb_dma_ownership.c` uses the actual Sound Blaster and 8237 code.
It injects a producer stop between the consumer's state check and its request
assertion. The old implementation fails because it transfers more PCM after
the stop. The fixed implementation passes for 8-bit DMA channel 1 and 16-bit
DMA channel 5, including a stale request, normal active transfers and refills
without consumer request assertions.

Run from the repository root with LLVM on Windows:

```powershell
clang -O2 -ffunction-sections -DCIRCLE_BUILD -DWYSE_BUILD=1 -DSOUND_FREQUENCY=48000 -Ipc -Iefi/platform tools/tests/sb_dma_ownership.c -Wl,/opt:ref -o work/sb_dma_ownership.exe
./work/sb_dma_ownership.exe
```

The hardware reproduction was Teenagent's intro on a Wyse 3040 at the
486/66 profile: braking outside the bank triggered roughly ten seconds of
broken audio. A RAM-capture diagnostic build masked the symptom even with
capture disabled, so its clean output was not accepted as proof of a fix.
The user confirmed clean HDMI audio with this correction alone, after all
temporary capture and reporting instrumentation had been removed.

The DMA playback-count experiment was reverted. This correction does not
change sample rates, buffer sizes, CPU speed or the guest's DOS configuration.
