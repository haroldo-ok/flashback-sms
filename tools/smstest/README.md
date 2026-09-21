Local copy of the devkitSMS skill's headless emulator, with two fixes so that
screenshots match real VDP behaviour (both matter for the gameplay sprites):

* only the first 8 sprites on a scanline are displayed; the 9th and beyond are
  dropped on that line, as the hardware does
* a background tile with the priority bit set hides sprites wherever its pixel
  is not colour 0

Rebuild: cc -O2 -o smstest smstest.c z80.c

It also has a sampling profiler, added to find where a frame's cycles go:

    profstart            # reset counters and start attributing cycles to the PC
    run 300
    profstop
    profdump prof.out    # "address cycles" per 32-byte bucket

Map the buckets onto the linker symbols in flashback.noi to get per-function
costs.  Note that .noi lists globals only, so a bucket covers the static
functions that follow it in the same file; temporarily removing `static` gives
exact attribution.
