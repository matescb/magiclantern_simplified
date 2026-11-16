# symgate - symbol-stability gate for the M50 Mark II ML port

Why: on this camera a core rebuild that moves symbol addresses has repeatedly
produced deterministic boot crashes, and modules resolved against a `.sym` that
does not belong to the running core crash at load. Working rule: keep the core
layout unchanged, change modules only. These host-side checks enforce that
before anything reaches a card. They prove nothing about camera behaviour.

## Files

| Path | Purpose |
|---|---|
| `baseline/M50mk2_101.sym` | `.sym` of the known-booting layout (md5 `d2f46ba5e091fa02d53a8aae9969442f`) |
| `baseline/BASELINE.json` | commit, md5s, anchors, ELF section ends, load address |
| `check_symbol_stability.py` | new `.sym` vs baseline: added / removed / moved, first divergence, shift, section ends |
| `check_module_syms.py` | module imports (`nm -u`) vs a core `.sym` |
| `check_card_image.py` | card dir or FAT image: does the `.sym` belong to the `autoexec.bin` next to it |
| `symlib.py` | shared parsing / diff helpers |
| `tests/test_symgate.py` | pytest, synthetic fixtures |

Needs `arm-none-eabi-nm`, `arm-none-eabi-readelf`; FAT images are read with `7z`
(no sudo, no mtools). Exit codes everywhere: 0 pass, 1 gate failed, 2 bad input.

## Use

```sh
G=/mnt/data/WORK/eos/tools/symgate
B=<tree>/platform/M50mk2.101/build

# (b) after every core build
python3 $G/check_symbol_stability.py $B/M50mk2_101.sym --elf $B/magiclantern

# (c) every module against the core it will run with
python3 $G/check_module_syms.py $B/zip/ML/modules/*.mo $B/M50mk2_101.sym

# (d) staging dir (build/zip), mounted card root, or FAT image
python3 $G/check_card_image.py $B/zip --elf $B/magiclantern
python3 $G/check_card_image.py sd.raw --workdir /mnt/data/WORK/eos/sim/<ID>
python3 $G/check_card_image.py $STAGE --write-manifest   # then copy $STAGE to the card

python3 -m pytest -q $G/tests -p no:cacheprovider
```

## What each gate decides

**check_symbol_stability** fails when any pre-existing symbol moved, a symbol was
removed (`--allow-removed`), a baseline anchor differs, or (with `--elf`) a
section start/end such as `_bss_end` changed (`--allow-section-change`). Added
symbols alone pass. `--ignore NAME` reports a move without counting it.

Note on `build_version`: `version.c` holds `build_id = "<commit> <branch>"`
right before `build_version`, so the *branch name length* changes the layout.
A pure rebuild on a branch whose name is shorter than `walkback-milestone`
moves `build_version` (-3 on `offline/C6-good`) or, once alignment padding is
used up, everything after it (53 symbols and all of `.rodata` by -8 on
`offline/C4`). Build on a branch name of the same length, or expect this.

**check_module_syms** fails on an import that neither the core `.sym` nor
another module on the command line defines. It checks names only, not that the
address behind the name is right - that is what the card check is for.

**check_card_image**: the ML core has no sym/core consistency check of its own
(`module_load_symbols` in `src/module.c` feeds every line of the `.sym` to TCC
unverified), and `autoexec.bin` embeds no hash of the `.sym`. What can be
compared from the two files alone:

1. the `.sym` filename against the `ML/MODULES/<name>.sym` string in the binary;
2. `build_user/build_date/build_id/build_version`: the core image sits
   uncompressed in `autoexec.bin`, so the bytes at the addresses the `.sym`
   claims must be exactly those C strings (they sit at the end of `.text`,
   after all code, so nearly any code-size change moves them);
3. the address-to-file delta derived from (2) must put the load address
   (`RESTARTSTART` 0xc78e0) on the core entry branch right behind the loader
   stub - this catches a `.sym` shifted uniformly;
4. Thumb prologue score: share of function symbols whose first instruction in
   the binary is a `push`. Matching pair: 0.45; shifted controls: <= 0.09.
   Scored per address range, so a part-way shift is located;
5. with `--elf`: exact equality with the symbol table regenerated from the ELF;
6. imports of every `.mo` on the card;
7. `ML/SYMGATE.TXT` md5 manifest if present (written by `--write-manifest`).

Checks 2-4 are strong evidence, not proof: a change that moves only data
symbols in `.data`/`.bss` (not code, not the version strings) is invisible to
them. Only (5) or (7) is exact.

## Proposal: make the pairing verifiable on the camera

Host side, no core change (available now): write `ML/SYMGATE.TXT` when staging
and verify it before every boot / after every WiFi deploy.

Core side (needs a core change, so it must go in together with a deliberate,
hardware-tested layout move): append a line `<crc32> ml_core_id` to the `.sym`
at build time, where the value is a CRC of the name/address list, patch the
same value into a fixed slot of `autoexec.bin` after linking, and have
`module_load_symbols` refuse to load modules when the two differ. A zero-cost
variant with no build change: after loading the `.sym`, compare
`tcc_get_symbol("build_date")` with `&build_date` and refuse on mismatch.
