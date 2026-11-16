# 36 — M50 II: WiFi build-deploy lane — feasibility analysis (2026-07-18)

**Goal:** push ML builds (autoexec.bin + .sym + .mo) to the camera over WiFi from the Linux
host — no card shuffle — and remotely reboot into the new build. **Scope guard:** ML files
only; Canon `.FIR` firmware is NEVER touched (1.0.1 stays, bootloader untouched → unbrickable;
recovery = card reader, same as today).

**Status: ANALYSIS ONLY — no code written.** Probed: Ghidra (gang100.bin, M50 II 1.0.1 ROM)
+ harness state. Camera was powered off during this session (last boot 17:37 stopped at big.2
mid-boot; `ver` at 18:09 unanswered), so live probes are deferred to Phase 0 below.

Companion docs: docs/29 (D8 reference lineage), docs/32 (FIO arc), docs/35 (FIO write fix).

---

## 1. Why WiFi (measured baselines)

| Channel | Throughput | 252 KB autoexec.bin (current: 257,696 B) |
|---|---|---|
| UART deploy (`tools/mlserial/mldeploy.py`, measured 2026-07-16) | ~15–21 B/s | **~4 h** — dead end. Bottleneck is the camera's tiny ch0 RX ring + shared TX register, NOT the 115200 baud (see mldeploy.py header). |
| Card shuffle (`flash.sh`) | fast | but door / pull / reader / reinsert / power-cycle every iteration |
| WiFi TCP (yolo-style, conservative ~1 MB/s on 2.4 GHz 11n) | — | **< 1 s**, card never leaves the camera |

Bonus: the same socket channel pulls raw/MLV captures off the card at MB/s (reverse direction),
replacing the crash-prone sustained serial dumps (docs/35 §5 warning).

## 2. What the Ghidra probe PROVED (gang100.bin, 1.0.1)

### 2.1 All WLAN/Lime eventproc names exist in this ROM

Name-string cluster (ascii, contiguous):

| name | addr | | name | addr |
|---|---|---|---|---|
| `wlanup` | e039a244 | | `NwLimeInit` | e039a27c |
| `wlandown` | e039a24c | | `NwLimeOn` | e039a28c |
| `wlanipset` | e039a264 | | `wlanpoweron` | e039a2e0 |
| `wlanchk` | e039a304 | | `wlanpoweroff` | e039a2f0 |

Supporting driver strings: `[WLAN] WlanUp OK` e0424834, `[WLAN] WlanDown OK` e0424c20,
`[NWCOM] AlreadyWlanPowerOn/Off` e0373114/e03730e0. (No static xrefs — the registration
table is consumed at runtime; irrelevant, because…)

### 2.2 The `call()` (eventproc-by-name) stub is ALREADY resolved on M50 II

`platform/M50mk2.101/stubs.S:77`: `THUMB_FN(0xE05CBF82, call)` — "[M50II 1.0.1 matched]".
⇒ The entire yolo-style bring-up sequence (`call("NwLimeInit")` → `call("NwLimeOn")` →
`call("wlanpoweron")` → `call("wlanup")` → `call("wlanchk")`) is invocable from ML **today**;
only live verification remains (Phase 0). Reference recipe:
`research/kitor_edmac_sx740/modules/yolo/yolo.c:340-435` (200D, HW-proven on D8).

### 2.3 Remote reboot primitive CONFIRMED

- Canon shim `FUN_e05b0104`: DebugMsg `"PROP_Request PROP_REBOOT"` (str e05b04dc) →
  loads prop ID literal **@e05b04f8 = 0x80010003** → `bl 0xe05bf86c` (PropMgr request,
  4-byte payload). **0x80010003 == ML `PROP_REBOOT`** (src/property.h:574, "used by
  firmware update code") — ML already ships `prop_request_change(PROP_REBOOT, &reboot, 4)`
  (src/debug.c:466 "Restart camera" path). PropMgr is canon-ready on M50 II (big.8 arc).
- Clean-restart wrapper candidate: `FUN_e0879076` = 3 shutdown-prep calls
  (e07066c4, e0706680, e0742f80) + shim(1). Second caller: FUN_e08b85f0.
- Live test options (Phase 0): `call 0xE05B0105 0` over serial (thumb bit set), or ML's
  own PROP_REBOOT request. Either reboots → autoexec reloads from card → mllog sees a new boot.

### 2.4 Network socket stack exists; addresses NOT yet resolved (~16 stubs to hunt)

- Canon net layers present: `PComNetSocket*` file strings e038a1dc–e038a278 (+ NetDomain
  copies), full **RTMP client** (YouTube live streaming) `rtmp_OpenSocket/ConnectSocket/
  CloseSocket` strings e05e1054–e05eeb60, TCP server strings (`RecvTcpSocket*`,
  `OpenTcpServerSocket`) e0e9716c–e0e98b08.
- **Resolution strategy** (Phase 1):
  1. This ROM references debug strings PC-relatively (`adr`, confirmed at e05b0108) ⇒
     referencing code sits within ±4 KB of its string. Disassemble the RTMP client
     neighborhood (~e05e8000–e05f0000), extract the DryOS socket callees from
     rtmp_OpenSocket/Connect/Send/Recv — they call the exact functions we need.
  2. Cross-check with 200D reference addrs (`research/*/platform/200D.101/stubs.S:439-460`):
     socket cluster e071d880–e071dac4 (create/bind/connect/listen/accept/recv/recvfrom/
     send/sendto/setsockopt/getsockopt), `socket_close_caller` e0714a20, `socket_convertfd`
     e07321f4, `socket_select_caller` e0714b1c, `wlan_connect` e036d59c, + `nif_setup`,
     `set_IP_address`. M50 II NWCOM strings sit at e036d–e0373 — same neighborhood as
     200D's wlan_connect ⇒ layouts are close in this region.
  3. Verify `struct wlan_settings` layout (0xfc bytes, `ml_socket.h`) against the M50 II
     `wlan_connect` decompile before use.
- **TRAP (do not repeat):** `"SocketServiceInstall: SUCCESS"` @e00afc04 / FUN_e00afaca is
  the **card-slot** ("socket") service from `Storage/CardServiceMgr.c` — NOT network. Wasted
  anchor; network work must anchor on PComNet/RTMP/NWCOM strings instead.

## 3. Existing assets this lane reuses

| Asset | Role |
|---|---|
| `research/kitor_edmac_sx740/src/ml_socket.h` | socket + wlan API decls, `wlan_settings` struct |
| `research/kitor_edmac_sx740/modules/yolo/yolo.c` | HW-proven D8 WLAN bring-up + TCP streaming recipe |
| `src/serial_shell.c` command loop | becomes the TCP shell (same commands, faster pipe) |
| `FIO_WriteFile` uncacheable gate (docs/35 §2) | card writes proven (4.27 MB single write) |
| `FIO_RenameFile` wrapper (src/fio-ml.c:509) | atomic-ish commit — **`_FIO_RenameFile` stub NOT yet in M50mk2 stubs.S** (grep-verified); resolve in Phase 1 or fall back to FIO_RemoveFile+rewrite |
| `tools/mlserial/mldeploy.py` | host-side protocol/UX starting point (CRC/verify flow) |
| `mllog.py` boot tracking | post-reboot build verification |

## 4. Proposed update dance (safety-first)

```
host: build → TCP push {manifest, autoexec.bin, sym, mo...} with per-file CRC32
cam:  receive to RAM (fio_malloc) → CRC check → write B:/AUTOEXEC.NEW → FIO_GetFileSize
      + readback CRC → rename AUTOEXEC.BIN→AUTOEXEC.BAK, AUTOEXEC.NEW→AUTOEXEC.BIN
      → ack → (optional) PROP_REBOOT
```
- Keep `AUTOEXEC.BAK` on card = instant reader-side rollback.
- Deploy only in idle/menu GUI state (EVF timing sensitivity, docs/35 §5).
- `wlandown` + Lime off after deploy (power + memory hygiene).

## 5. Phased plan (each phase independently useful)

- **Phase 0 — live probes, zero code** (next lab session, existing harness):
  1. `call("wlanchk")` (status probe), then NwLimeInit→NwLimeOn→wlanpoweron→wlanup while
     watching `[NWCOM]/[WLAN]` UART lines for "WlanUp OK".
  2. Memory headroom before/after Lime bring-up (shell reads; Canon free ≈ 490 KB baseline —
     Lime/net buffer appetite is the #1 go/no-go).
  3. Reboot test: `call 0xE05B0105 0` → expect clean reboot, mllog new boot.
  4. Repeat 1–2 in photo/menu vs movie-LV standby (mode exclusivity check).
- **Phase 1 — stub resolution (Ghidra, ~16+2 stubs):** socket cluster via RTMP-callee
  extraction + 200D shape match; `wlan_connect`/`nif_setup`/`set_IP_address`; `_FIO_RenameFile`;
  verify wlan_settings layout. Static-verify per tools/verify_stubs.py discipline.
- **Phase 2 — camera side:** port ml_socket.h; `wifi_deploy` task = bring-up → TCP listen →
  receive/CRC/write/rename → ack → optional reboot; reuse serial_shell dispatch over the
  socket; teardown. Gate M50mk2-only.
- **Phase 3 — host side:** `tools/mlserial/mlwifi.py` (push client + shell), flash.sh
  integration: build → push → reboot → `mllog boots` confirm.
- **Phase 4 — extras:** multi-file manifest (sym/modules), capture pull-back (RAW/MLV → host),
  maybe screen streaming (yolo heritage, debugging EVF states remotely).

## 6. Risks / open questions

| Risk | Weight | Mitigation |
|---|---|---|
| Lime core + net buffers blow the ~490 KB Canon heap budget (pages=4 panic precedent, memory notes) | **HIGH** | Phase 0 measures BEFORE any code; bring-up on-demand only, teardown after deploy |
| wlan bring-up perturbs EVF/raw pipeline timing | MED | deploy in idle GUI state only; it's a dev tool, not a recording-time feature |
| `wlan_settings` layout differs from 200D | MED | verify against M50 II wlan_connect decompile before first call |
| multi-write-to-one-handle bug (mldeploy.py note) recurs camera-side | LOW | was a serial-path artifact; mlv_lite frame-write retest (docs/35 §6) will confirm the pattern; fallback = single-write per file from RAM |
| WPA2 creds on card (plaintext config like yolo) | LOW | dev-LAN only; document; optional AP-mode later |
| UART deploy improvements instead? | — | rejected: RX-ring ceiling is architectural (mldeploy.py header); WiFi is orders of magnitude faster |

**Boot time still bounds the loop:** ~30 s cold boot per iteration regardless of transfer speed.

## 7. Live-probe checklist status

- [x] Harness/daemon alive (mlserald running, uart.log current)
- [ ] Camera reachable — **NO (powered off 17:37); all Phase-0 items pending next lab session**
- [x] Ghidra: eventproc names present (§2.1)
- [x] Ghidra: call stub already matched (§2.2)
- [x] Ghidra: PROP_REBOOT = 0x80010003 confirmed + ML-side support exists (§2.3)
- [x] Ghidra: socket/RTMP/NWCOM stack located; resolution strategy defined (§2.4)
