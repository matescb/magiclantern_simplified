
## Limitations

- UART timestamps are host arrival times of serial read batches (median gap 0.4 s, see rates table). Intervals under 1 s are
  good to about +/-0.4 s; the write rate in particular is only a lower bound (14 MB inside one or two batches).
- raw_vidx `frames` are counted from log lines (`cbr_w: give sem`, distinct `wk.dst`); logs are rate-limited from module
  81fd5d54 on, so counts are lower bounds. The `give sem` cadence measures the whole per-frame cycle (wait for frame + copy +
  logging), so the copy rate is a lower bound on memcpy speed.
- Build identity is the AUTOEXEC.BIN size only. Module binaries, `ML/SETTINGS`, and module-enable files are invisible in the log;
  several findings above (1, 4, 7) hinge on exactly those.
- Boot segmentation relies on `[ML] pre.0`; a boot whose first lines were lost (daemon down, garbled UART) is merged into the
  previous one. A boot that was simply powered off before big.8 by a bench loop counts as "stalled".
- Signature = (pc, lr). RAM-address signatures (`0x000Dxxxx`, `0x010xxxxx`, `0x19xxxxxx`) shift with every build and are
  not comparable across builds; the same bug may appear under several rows.
- "Last ML phase" is the last `pre./post./big.` marker printed before the abort in that boot; `+rec` means a recording marker
  was printed less than 120 s earlier.
- The docs/memory cross-check covered MEMORY.md, the memory/*.md notes and docs/30-37 by keyword search, not a full read.
- The unit tests cover the parser on synthetic input; the real-log tables were spot-checked by hand (abort dump at db boot 8,
  attempt at db boot 212), not exhaustively.

## Next hardware checks

1. `0xE021B8FC`: on the card that reproduces it, remove/rename the module-enable and settings files one at a time (start with
   `raw_vidx.en`) with the SAME AUTOEXEC.BIN and cold-boot 5x each. Expect 5/5 crash to flip to 0/5 on one file. Record
   `md5sum` of AUTOEXEC.BIN and every .mo before each run so the next log has a real build key.
2. Print a build id over UART at pre.0 (core git hash) and at each module load (module md5). Without it the log cannot separate
   core from module regressions.
3. On the walkback core (a10b16c23): 20 cold boots, count `0xE02F281A` / `0xE00436FC` / `0xE05D1ED0` at big.4a/big.4a2. The log
   predicts roughly 1 in 3 boots lost to this family unless the empty-`normal_code_patches` change is in.
4. raw_vidx stop path: after `FIO_WriteFile done`, capture which pointer is NULL (`EvfDevState.c:372/542` burst, then `pc=0`).
   Dump the EVF state object at 0x10108 hooks and the EDMAC CBR slots before releasing LV raw; test closing the file and
   returning without `raw_lv_release`.
5. Copy rate: time the frame copy on-camera with the microsecond timer (not UART timestamps) for 1 frame of 2,809,856 B from
   the uncacheable source and from the cacheable alias. The log gives about 0.5 s per frame cycle (about 2 fps); 24 fps at this
   crop needs about 67 MB/s.
6. Write rate: write 64 MB in one `FIO_WriteFile` and time it on-camera; the log only bounds it to >= 17 MB/s.
7. Silent restart at post.1: log the plug state and power-button state for 10 cold starts to see whether every plug-on boot
   double-starts, and whether a battery power-on does too.
8. `0xE005DE52` at big.5.3: it exists on slow cores too, so test the font load with a retry or a later start rather than
   slowing the boot.
