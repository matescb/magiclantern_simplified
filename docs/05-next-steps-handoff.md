# Next steps & hand-off

## Immediate options
1. **Finish the port ourselves (weeks):** build a capstone stub-finder to batch ~90 stubs.S entries,
   then the consts.h structs, then build M50mk2.101 autoexec.bin, then (risky) hardware bring-up.
2. **Hand off to the ML devs (fastest path to a real port):** we now have BOTH a clean 1.0.1 dump and
   a running head start (boot consts). Deliver to kitor / names__are__hard on the ML Discord.

## What to deliver to kitor
- The dump: `m50ii_dumper/dump_1.0.1/gang100.bin` + `gang200.bin` (48MB total; over Discord's limit -> use a file host).
- `m50ii_dumper/PORT_PROGRESS.md` and `docs/03-port-progress.md` (the found boot constants).
- Note: firmware **1.0.1** (likely newer/lower than their 1.0.3 dump), real ROM1 captured.

## Draft message (to kitor / #m50)
> Hi — I have an EOS M50 Mark II on **firmware 1.0.1** and pulled a full Canon Basic ROM dump
> (gang100 32MB ROM0 + gang200 16MB real ROM1, model strings confirm M50 Mark II, DryOS 2.3).
> I also started a baseline port: created platform/M50mk2.101 from M50.110 and decoded the 7 boot
> patch points for 1.0.1 (BR_* macros in consts.h). Happy to share the dump (file host) + my notes,
> run diagnostics, or hand the camera over. What's most useful? I know 1.0.4 is locked; I'm on 1.0.1
> and will NOT update it.

## Hard rules
- NEVER update the camera firmware (keep 1.0.1).
- Hardware bring-up (running autoexec.bin) needs the bootflag enabled and carries BRICK RISK —
  do it with kitor's recovery guidance; Canon Basic `boot_dis.m` can disable the bootflag.
- Revoke the Discord token that passed through chat: `/mnt/data/WORK/eos/.discord_token` (still live).
