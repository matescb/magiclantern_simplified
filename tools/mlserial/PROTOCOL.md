# mlserial wire protocol (host <-> M50 II serial_shell)

Link: `/dev/ttyUSB0`, 115200 8N1, raw. Camera side = `src/serial_shell.c`
(task `ml_serial`). Host side = `mlserald.py` (daemon) + `mlserial.py` (CLI).

## Framing

**Host -> camera** (one command per line):
```
@<seq> <command args...>\n
```
`@<seq>` is optional but the daemon always sends it, so replies can be correlated.

**Camera -> host**:
- The camera echoes the received line (starts with `@` or bleeds after `ml> `).
- Command output: free-form body lines. Structured payloads use a leading tag:
  - `D <addr> <hexbytes>`  — a `dump` data row (32 bytes/line, little-endian)
  - `=crc 0x........`       — CRC32 of all dumped bytes (poly 0xEDB88320 = zlib)
  - `W <i> <val>` / `W final <val>` — a `watch` sample
- Terminator (always last):
  - `=ok @<seq>`
  - `=err @<seq> <message>`
- Asynchronous firmware logs keep the `[ML] ...` prefix and are **never** part of a
  command body — the daemon routes them to the log only.

The daemon collects every non-`[ML]`, non-echo, non-prompt line between sending a
command and its `=ok/=err` terminator as that command's response body.

## Commands (camera)

| command | effect |
|---|---|
| `help` | list commands |
| `ver` | build / camera id |
| `md <addr> [nwords]` | dump 32-bit words (default 0x10, max 0x100) |
| `rd <addr>` | read one 32-bit word |
| `dump <addr> <len>` | bulk read -> `D` lines + `=crc` (max 0x100000) |
| `edmac` | dump raw-LV EDMAC block (0xd0420200) |
| `watch <addr> [ms] [n]` | sample a word over time, report changes |
| `key <code>` | inject a GUI keycode via m50ii_handle_canon_gui_event(0,code,0) |
| `wr <addr> <val>` | write a 32-bit word — **refused for addr >= 0xE0000000 (ROM)** |
| `call <addr> [a b c d]` | call a function pointer, print return |
| `load <hexbytes>` | stage a blob into the RAM scratch buffer |
| `exec [a b c d]` | cache-sync + call the scratch blob (thumb), print return |

Safety rail: the only forbidden operation is **writing to ROM** (`wr`/`load` reject
targets >= 0xE0000000). Everything else is live; a bad RAM/MMIO write or call faults
at worst and is recovered by a cold boot.

## Control socket (host, newline-delimited JSON)

`{"op":"cmd","text":"...","timeout":N}` -> `{"status":"ok|err|timeout","body":[...],"seq":N,"msg":""}`
`{"op":"raw","text":"..."}` · `{"op":"status"}` · `{"op":"ping"}`

Socket path: `/tmp/mlserial-<md5(RUN_DIR)[:10]>.sock` (short, AF_UNIX limit-safe).
