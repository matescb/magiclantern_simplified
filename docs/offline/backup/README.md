# Magic Lantern for Canon EOS M50 Mark II — Project Docs

Working dir: `/mnt/data/WORK/eos/`
Goal: bring Magic Lantern to the **EOS M50 Mark II** (Digic 8). We own a physical M50 II on **firmware 1.0.1**.

Status (2026-07-07): **verified full 1.0.1 ROM dump obtained**; baseline port **started**; bootable-card execution is confirmed; LED diagnostics have crossed Canon context restore and identified the current blocker as the first-init helper path before ML `my_init_task`.

## Layout
| Path | What |
|---|---|
| `docs/01-ml-m50ii-status.md` | Research: where ML stands for the M50 II (community/Discord/GitHub findings) |
| `docs/02-rom-dump-procedure.md` | How we dumped the 1.0.1 ROM (reproducible) |
| `docs/03-port-progress.md` | The port: build validation, skeleton, boot consts, method, TODO |
| `docs/04-build-and-tooling.md` | Environment, toolchain, build + RE commands |
| `docs/05-next-steps-handoff.md` | What's next + kitor hand-off + draft message |
| `docs/06-stubs-ledger.md` | Stub-finding ledger for the M50 II port |
| `docs/07-discord-findings.md` | Mined ML Discord findings for Digic 8 handoff, QEMU, UART, and raw-video clues |
| `docs/08-boot-trace-report.md` | Hardware boot-trace report: Canon Basic attempts, bootflag, LED diagnostics, context-restore proof, current first-init-helper blocker |
| `m50ii_dumper/` | Dumper card tooling + the ROM dump (`dump_1.0.1/`) |
| `m50ii_dumper/dump_1.0.1/` | `gang100.bin` (ROM0 32MB) + `gang200.bin` (ROM1 16MB) + `SHA256SUMS.txt` |
| `magiclantern_simplified/` | ML source (reticulatedpines fork); `platform/M50mk2.101/` = our WIP port |

## One-line summary
No public ML exists for the M50 II. We have a clean **1.0.1** dump, a WIP `M50mk2.101` port, proven `AUTOEXEC.BIN` execution, and a narrowed boot blocker in Canon's first-init-helper to ML-init handoff.
