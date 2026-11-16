# ROM dump procedure — EOS M50 Mark II (Canon Basic) — REPRODUCIBLE

We used the **Canon Basic** dumper (NOT the portable ROM dumper). On Digic 8 the portable dumper
only gets ROM0 (ROM1 = zeros); **only Canon Basic dumps the full ROM (gang1xx + gang2xx)**.
Canon Basic is also non-destructive — it does not touch the camera bootflag.

## Card requirements (from CHDK wiki + kitor's tool)
- **FAT32** (SD/SDHC <=32GB, or SDXC formatted FAT32; exFAT will NOT work for the SCRIPT flag).
- Boot-sector flags **EOS_DEVELOP + BOOTDISK + SCRIPT** (SCRIPT string at offset 0x1F0).
- Root files:
  - `extend.m` — the Canon Basic dumper script.
  - `script.req` — **MUST contain exactly `for DC_scriptdisk`** (no trailing newline). EMPTY = camera ignores the card (this was our first failure; camera just created DCIM folders).

## The dumper script (`extend.m`)
```
private sub Initialize()
    if System.Create() = -1 then
        SystemEventInit()
    end if
    r = ExecuteEventProcedure("SaveAllRomImageToFile")
    if r = -1 then
        ExecuteEventProcedure("SaveBootableToFile")
    end if
    f = Fopen_Fut("A/DUMPDONE.TXT", "w")
    if f <> 0 then
        Fwrite_Fut("done\n", 5, 1, f)
        Fclose_Fut(f)
    end if
end sub
```

## Automated card prep (Linux)
`m50ii_dumper/prepare_card.sh <disk>` (default /dev/sdb): safety-guarded (removable + <64GB only; aborts
on data unless FORCE=YES). Formats FAT32, copies extend.m + script.req, sets flags via
`magiclantern_simplified/tools/card-flags/edit_card_flags.py <part> --eos-develop --bootdisk --script`.
Verify flags: SCRIPT@0x1F0, EOS_DEVELOP@71, BOOTDISK@92.

## On the camera
1. Do NOT update firmware (keep 1.0.1). Insert card.
2. Power on -> **Playback (>) mode** -> press **FUNC./SET**.
3. Screen freezes while dumping ~48MB (~30-90s). Wait for card LED to stop. Power off.

## Output & verification
- `gang100.bin` = 32MB ROM0 (loads at 0xe0000000; main fw at 0xe0040000; ARM boot code; DryOS 2.3).
- `gang200.bin` = 16MB ROM1 (real data, not zeros).
- Verify: entropy > ~4 (not blank), dump twice -> md5 must match. Our dump: see `dump_1.0.1/SHA256SUMS.txt`.
- Model strings confirm "Canon EOS M50 Mark II" / "M50m2" / "Kiss M2".
