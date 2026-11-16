# 27 — Menu input via Canon task-switch callback API (registration, not squatting)

Status: **PLAN — awaiting confirmation.** Camera safe on iter5 (849b7eed).
Supersedes the iter6–8 dead end (squatting `*0x4144`, see docs/26 §9).

## Why this path
ML needs its `ml_gui_main_task` to run in place of Canon's `GuiMainTask` so button
events reach ML. The mechanism is the standard ML `TASK_OVERRIDE` (swap `next_task->entry`
just before the task first runs). That swap must be driven from a per-task-switch callback.

On DIGIC-7 siblings (M50/R/RP) ML installs its hook into a **free** scheduler slot.
On M50 II that slot (`*0x4144`) is **occupied by Canon's own callback-list walker** —
overwriting it starved Canon's scheduler → Module-64 pool panic (iters 6/7/8, all HW-confirmed).
There is **no free slot** (`0x4148`=Canon slot-2, `0x414c`=UART global). So instead of
squatting the walker, ML **registers its own node into the list the walker walks** — the
"use Canon's wheel" approach the user asked for.

## RE — the Canon callback list (all verified this session, gang100.bin)

### Control block (RAM `0x00023e3c`, reached via ROM literal `DAT_e01642b0`)
| off  | meaning                                                        |
|------|----------------------------------------------------------------|
| +0x0 | inner spinlock word (LDREX/STREX; taken by the walker)         |
| +0x4 | outer lock word (IRQ-disabled spinlock; `FUN_e021b8f0`)        |
| +0xc | **sentinel node** (embedded): sentinel.next=+0xc, .prev=+0x10  |
| +0x10| sentinel.prev == list tail; `==0` means "list never inited"    |

### Node layout (12 bytes) — confirmed from walker disasm
| off  | field  |
|------|--------|
| +0x0 | next   |
| +0x4 | prev   |
| +0x8 | fn (thumb addr) |

### Walker `FUN_e0163fe4` (installed at `*0x4144`)
Scheduler calls `(*0x4144)(1, old_task, next_task)`. Walker then:
```
acquire inner spinlock (base+0x0)
for (node = base[+0x10]/*tail*/; node != &sentinel; node = node->prev)
    node->fn(1, old_task, next_task)        // all 3 args passed UNCHANGED
release inner spinlock
```
=> A registered callback's signature is `void cb(int one, task *old, task *next)`,
invoked inside the scheduler spinlock. (This is exactly why iter7's UART-in-callback
tripped the spinlock-timeout: no blocking/UART/alloc is allowed here.)

### Register primitive `FUN_e012c6f4` (called once by Canon via `FUN_e012c530`)
Inserts Canon's own fixed node (RAM `0x000a8efc`) at the list head under nested locks:
```
sentinel = base + 0xc
if (base[+0x10] == 0) { base[+0x10] = &sentinel; sentinel.next = &sentinel; }  // lazy init
node.prev = 0; node.next = 0;
FUN_e021b8f0(base + 0x4);                 // OUTER lock: disables IRQ + spinlock on base+0x4
  <inner spinlock base+0x0 = 1; DMB>
  node.next          = sentinel.next;
  sentinel.next.prev = &node;
  sentinel.next      = &node;
  node.prev          = &sentinel;
  <DMB; base+0x0 = 0; DSB>
if (base[+0x10] != &sentinel)             // list non-empty -> make sure walker is live
    FUN_e01669a4(0, 0xe0163fe4);          // writes *0x4144 = walker
<DMB; base+0x4 = 0; DSB>                   // release OUTER lock
```
`FUN_e012c6f4` can only insert Canon's fixed node — not usable directly. ML replicates
this exact locked sequence with its own static node.

## ML implementation

### Files
| File | Action | Why |
|---|---|---|
| `platform/M50mk2.101/consts.h` | UPDATE | add `TASK_SWITCH_CBLIST_BASE 0x23e3c`, `TASK_SWITCH_WALKER 0xe0163fe4` |
| `platform/M50mk2.101/stubs.S` | UPDATE | `NSTUB` for `cblist_outer_lock`(e021b8f0) + `install_hook_slot`(e01669a4); remove the dead `DATA_PTR(0x4144, task_dispatch_hook)` (we no longer write 0x4144 directly) |
| `src/init.c` | UPDATE | replace dead `m50ii_task_dispatch_hook`/0x4144-squat with a static node + `m50ii_cb` + `m50ii_cblist_register()` at pre.5 |

### `src/init.c` shape
```c
struct cbnode { void *next, *prev; void (*fn)(int, struct task*, struct task*); };
static struct cbnode m50ii_cbnode;                 // ML's list entry (static, never freed)

// CRITICAL SECTION — scheduler spinlock held. NO UART / alloc / blocking.
static void m50ii_cb(int one, struct task *old, struct task *next) {
    if (ml_started || !next) return;
    struct context *ctx = next->context;
    if (!ctx || ctx->pc != (uint32_t)task_trampoline) return;
    for (m = _task_overrides_start; m < _task_overrides_end; m++)
        if ((void*)next->entry == m->orig) {
            next->entry = m->replacement;
            m50ii_override_name = next->name;      // plain globals, read by heartbeat (safe ctx)
            m50ii_override_count++;
            break;
        }
}

static void m50ii_cblist_register(void) {          // mirrors FUN_e012c6f4, ML node
    int *base = (int*)TASK_SWITCH_CBLIST_BASE;
    int *sentinel = base + 3;                       // base + 0xc
    m50ii_cbnode.fn = m50ii_cb;
    if (base[4] == 0) { base[4] = (int)sentinel; sentinel[0] = (int)sentinel; }
    cblist_outer_lock(&base[1]);                    // FUN_e021b8f0(base+0x4)
      <inline-asm inner spinlock base[0]; DMB>
      m50ii_cbnode.next = (void*)sentinel[0];
      ((struct cbnode*)sentinel[0])->prev = &m50ii_cbnode;
      sentinel[0] = (int)&m50ii_cbnode;
      m50ii_cbnode.prev = sentinel;
      <DMB; base[0]=0; DSB>
    if (base[4] != (int)sentinel) install_hook_slot(0, TASK_SWITCH_WALKER); // FUN_e01669a4
    <DMB; base[1]=0; DSB>                            // release outer lock
}
```
Gate: `#define M50II_REGISTER_CB 1` (on for the HW test; QEMU-gate first).

## Pre-write RE checks (cheap, before touching code)
1. IRQ-restore pairing: `FUN_e021b8f0` disables IRQ and does not re-enable; confirm how
   `FUN_e012c530`/`FUN_e012c6f4` restore IRQ state (return-value CPSR? implicit `base[1]=0`?).
   Mirror it exactly — a lost IRQ-enable hangs the camera.
2. Arg order: confirm scheduler `FUN_e01b1d82` passes `(1, old, next)` (swap target = `next`).
3. Thumb bits for the two NSTUBs.

## Risks
| Risk | Likelihood | Mitigation |
|---|---|---|
| Nested-lock / LDREX-STREX / IRQ mishandling -> hang or list corruption -> scheduler panic (iter8-class) | MED | copy exact disasm semantics; do the 3 pre-write checks; QEMU-gate before HW |
| Callback does too much in critical section (iter7-class spinlock timeout) | LOW | `m50ii_cb` is pure compare+store; no UART/alloc/block |
| QEMU can't create GuiMainTask -> override never fires there | — | expected; QEMU only proves "registration + boot, no regression"; HW proves the override |
| Regression on HW | LOW-MED | instant on-card revert to `AUTOEXEC.iter5-gui-849b7eed.BIN` |

## Validation
- `make` clean.
- QEMU: boot -> big.8, stable heartbeat, `ovr=`/`last=` line present, no panic (override count may be 0 — OK).
- HW: cold-boot -> OSD -> press INFO/arrows/SET/MENU/trash -> expect `m50ii_cb: overrode GuiMainTask`
  (via heartbeat counter, not from inside the callback), then `gui.ev type/param` keycodes, then menu.

## Complexity: Medium
RE is done. One new ~30-line function with a carefully-copied lock, 3 files, no HW-blind guesses.
