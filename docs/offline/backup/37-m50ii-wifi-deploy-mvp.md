# 37 — M50 II: WiFi build-deploy MVP (implementation)

**Goal:** push a freshly-built `AUTOEXEC.BIN` to the camera over WiFi and boot into it —
**card never leaves the camera.** Kills the card-shuffle dev loop (open door / pull / reader /
`cp` / reinsert / power-cycle) that dominated iteration cost. Follows the feasibility analysis in
docs/36.

**Scope guard:** ML files only. Canon `.FIR` is NEVER touched (1.0.1 stays, bootloader untouched →
unbrickable; recovery = card reader + the kept `AUTOEXEC.BAK`). ROM addresses stay local.

**Status (2026-07-19):** host tooling + camera module + build wiring DONE and host-side verified.
Blocked only on: (1) 9 socket/WLAN stub addresses (Ghidra, in progress), (2) the Phase-0 live WLAN
bring-up probe (needs the camera booted), (3) the end-to-end HW round.

---

## 1. Architecture

- **Shared 2.4 GHz WPA2 LAN**, host at a known IP. Camera's WLAN is 2.4 GHz only.
- **Camera = TCP client** (dials the host). Needs only `socket_create/connect/recv/send/
  convertfd/close` — no `bind/listen/accept/select` — and reuses the HW-proven D8 connect path from
  `modules/yolo/yolo.c` (200D). Host runs a tiny always-on TCP server.
- **Trigger = serial** (bootstrap): the existing serial harness injects a REC key into ML's
  module-key path; the `wifidep` module's `CBR_KEYPRESS` starts the pull.
- **Camera logic = a MODULE** (`wifidep.mo`). The only core change is adding absolute-symbol stubs
  to `stubs.S`, which emit **zero `.text`** (`THUMB_FN` = `name = addr|1`) → cannot re-trigger the
  symbol-shift boot crash. All deploy logic is module-only.

## 2. Wire protocol (camera dials host; all app ints little-endian)

```
cam -> srv : "MLWF"                                  (4-byte hello)
srv -> cam : "MLWF" + u32 file_count
srv -> cam : per file -> u32 name_len, name[name_len] (on-card path, no drive letter),
                         u32 file_len, u32 crc32 (zlib), file_len bytes
cam -> srv : per file -> u32 status (0=OK), u32 recv_crc
cam        : after last ack, close; then (optionally) PROP_REBOOT
```

CRC32 = zlib poly 0xEDB88320 both sides. Camera writes each file to `B:/<name>.TMP`, size-verifies,
then renames `<name>`→`<name>.BAK`, `<name>.TMP`→`<name>` (atomic-ish commit + rollback copy). The
`AUTOEXEC.BIN` write uses the proven uncacheable `FIO_WriteFile` gate (docs/35) with a single write
from a `sync_caches()`'d buffer.

## 3. Files

| File | Role |
|---|---|
| `modules/wifidep/wifidep.c` | camera module: config → WLAN up → connect → recv/CRC/write/rename → ack → teardown → reboot |
| `modules/wifidep/wifidep.h`, `Makefile` | module header (network_config, status codes) + build |
| `platform/M50mk2.101/stubs.S` | **+9 socket/WLAN stubs** (pending Ghidra addresses) |
| `modules/Makefile` (`ML_MODULES`), `platform/M50mk2.101/modules.included` | build + ship `wifidep` |
| `tools/mlserial/mlwifi.py` | host TCP server / pusher (`serve`, `crc`, `mkcfg`) |
| `tools/mlserial/test_mlwifi.py` | host loopback self-test (5/5 green) |
| `tools/flash-wifi.sh` | build → serve `--once` → serial-trigger → confirm new boot via `mllog` |
| `ML/SETTINGS/WIFI.CFG` (card) | creds/IPs; generate with `mlwifi.py mkcfg` |

Reuses: `src/ml_socket.h` (socket/WLAN API + `wlan_settings` 0xfc, already in-tree), `yolo.c`
bring-up + config parser, resolved FIO stubs (`_FIO_CreateFile/WriteFile/RenameFile/RemoveFile/
GetFileSize64`), `_prop_request_change`+`PROP_REBOOT`=0x80010003, `m50ii_uart_str`,
`handle_module_keys`@0xcd515.

## 4. The 9 stubs still to resolve (M50 II 1.0.1) — 200D reference shapes

`socket_create`(e071d880) · `socket_connect`(e071d8f2) · `socket_recv`(e071d99a) ·
`socket_send`(e071da20) · `socket_convertfd`(e07321f4) · `socket_close_caller`(e0714a20) ·
`wlan_connect`(e036d59c) · `nif_setup`(e07159e8) · `set_IP_address`(e0715828).
Resolve via RTMP-client-neighborhood callee extraction + 200D cluster shape match; verify
`wlan_settings` 0xfc layout vs the M50 II `wlan_connect` decompile. Insert as a new
`/** Network **/` block before `/** GUI timers **/` in `stubs.S`.

## 5. Phase 0 — free live WLAN probe (zero rebuild, any big.8 boot)

`call` (0xE05CBF82) is resolved and the eventproc name strings are at known ROM addresses, so drive
the whole bring-up over the serial shell before writing/flashing anything:
```
call 0xE05CBF82 0xe039a304   # wlanchk
call 0xE05CBF82 0xe039a27c   # NwLimeInit   (wait ~2s)
call 0xE05CBF82 0xe039a28c   # NwLimeOn
call 0xE05CBF82 0xe039a2e0   # wlanpoweron
call 0xE05CBF82 0xe039a244   # wlanup       -> watch UART for "[WLAN] WlanUp OK" (@e0424834)
call 0xE05CBF82 0xe039a24c   # wlandown     (teardown)
call 0xE05CBF82 0xe039a2f0   # wlanpoweroff
```
**GATE:** measure Canon free memory before/after Lime bring-up (the #1 HIGH risk — Lime + net
buffers vs the ~490 KB budget). If WlanUp OK + memory survives → proceed. If not, reconsider
(smaller footprint during deploy / AP-mode).

## 6. How to use (once stubs land + Phase 0 passes)

```
# one-time: make the card config (dotted IPs -> the decimal-uint32 format the parser wants)
python3 tools/mlserial/mlwifi.py mkcfg \
    --ssid MyNet --pass secret \
    --client 192.168.1.90 --server <host-ip> --gateway 192.168.1.1 --port 5455 --out WIFI.CFG
# copy WIFI.CFG to the card as ML/SETTINGS/WIFI.CFG (one physical flash, alongside the deploy build)

# then, forever after (card stays in the camera, camera in PHOTO/idle mode):
./tools/flash-wifi.sh --note "my change"
#   builds -> serves autoexec.bin -> injects REC key over serial -> camera pulls + writes + reboots
#   -> mllog confirms a new boot on the new md5
```

**Success = a second, different build md5 boots with the card still in the camera.**

## 7. Notes / risks

- `wifidep` defers the REC/LV key to `raw_vidx` while in movie mode (`is_movie_mode()` guard), so
  the two modules coexist; deploy from PHOTO/idle. If the REC injection turns out not to reach
  module CBRs in photo mode on HW, fall back to dropping `raw_vidx` from the deploy build.
- Boot (~30 s) still bounds each iteration; WiFi removes the *card handling*, not the boot time.
- WPA2 creds are plaintext on the card (dev-LAN only).
