#!/usr/bin/env python3
"""hwprobe - read-only hardware probe pack for the M50 II Magic Lantern port.

Runs the probes collected from the offline sprint reports (probes.json) through the mlserial
daemon and writes one timestamped JSON + markdown result per run.  See docs/offline/G3.md.

    hwprobe.py plan   --sym build/magiclantern.sym                 # what would run, gated items listed
    hwprobe.py run    --sym ... --dry-run                          # exact command sequence, no camera
    hwprobe.py run    --sym ... --stages 0,1 --label photo-lv      # real run (daemon must be up)
    hwprobe.py run    --sym ... --replay canned.json               # parser/evaluator against canned replies
    hwprobe.py after  --result results/hwprobe-....json FILES...   # mlv_check / rvi_check on card files

Defaults are reads of plain RAM only.  `call` needs --allow-call, `wr` needs --allow-wr, MMIO /
page-table reads need --allow-risky-read; gated steps are always printed in the plan first.
Shell syntax used (src/serial_shell.c): `rd <addr>` one word, `md <addr> <nwords>` (max 0x100),
`dump <addr> <len>` (D lines + =crc), `call <addr> [a b c d]` -> `ret 0x........`.
"""
import argparse
import ast
import datetime
import hashlib
import json
import math
import os
import re
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
MLSERIAL_DIR = os.path.join(TOOLS, "mlserial")
DEFAULT_PROBES = os.path.join(HERE, "probes.json")
DEFAULT_OUT = os.path.join(HERE, "results")
DEFAULT_UARTLOG = os.path.join(MLSERIAL_DIR, "run", "uart.jsonl")
MD_MAX_WORDS = 0x100          # SH_MD_MAX in serial_shell.c
MIN_SAMPLE_INTERVAL = 0.2     # the daemon polls the port every 200 ms (mlserald.py)
READ_RETRIES = 2
RISK_FLAGS = {"call": "allow_call", "wr": "allow_wr", "risky_read": "allow_risky_read"}
WORD_ROW = re.compile(r"^0x([0-9A-Fa-f]{8}):((?:\s+0x[0-9A-Fa-f]{8})+)\s*$")
CALL_RET = re.compile(r"^ret\s+0x([0-9A-Fa-f]{8})\s*$")
ML_RAM_LO, ML_RAM_HI = 0x00040000, 0x02000000   # window in which .sym addresses count as ML RAM


class ProbeError(Exception):
    """A step could not be carried out (bad reply, command error)."""


class Unresolved(Exception):
    """An address depends on a value that has not been read (dry run)."""


class Skip(Exception):
    """A 'require' step said the probe does not apply in this state."""


class Abort(Exception):
    """The shell stopped answering; the run ends and partial results are written."""


# ---------------------------------------------------------------- symbols + expressions
class SymTable:
    def __init__(self, path):
        self.path = path
        self.by_name = {}
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2:
                    try:
                        self.by_name.setdefault(parts[1], int(parts[0], 16))
                    except ValueError:
                        continue
        if not self.by_name:
            raise SystemExit("no symbols in %s" % path)
        self.sorted = sorted((a, n) for n, a in self.by_name.items() if ML_RAM_LO <= a < ML_RAM_HI)

    def sha1(self):
        with open(self.path, "rb") as f:
            return hashlib.sha1(f.read()).hexdigest()

    def addr(self, name):
        if name not in self.by_name:
            raise ProbeError("symbol %r not in %s" % (name, self.path))
        return self.by_name[name]

    def ml_range(self):
        return (self.sorted[0][0], self.sorted[-1][0]) if self.sorted else (0, 0)

    def nearest(self, addr):
        best = None
        for a, n in self.sorted:
            if a > (addr | 1):
                break
            best = (a, n)
        return "%s+0x%X" % (best[1], (addr & ~1) - (best[0] & ~1)) if best else None


_BIN = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b, ast.BitAnd: lambda a, b: a & b,
        ast.BitOr: lambda a, b: a | b, ast.LShift: lambda a, b: a << b, ast.RShift: lambda a, b: a >> b}


def evaluate(expr, variables, syms):
    """Evaluate an address/value expression: ints, + - & | << >> ~, sym("name"), var, var[i]."""
    if isinstance(expr, int):
        return expr

    def var(name, idx):
        if name not in variables:
            raise ProbeError("expression %r uses unknown variable %r" % (expr, name))
        if variables[name] is None:
            raise Unresolved(name)
        return variables[name][idx]

    def ev(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in _BIN:
            return _BIN[type(node.op)](ev(node.left), ev(node.right)) & 0xFFFFFFFF
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Invert):
            return ~ev(node.operand) & 0xFFFFFFFF
        if isinstance(node, ast.Name):
            return var(node.id, 0)
        if isinstance(node, ast.Subscript) and isinstance(node.value, ast.Name):
            return var(node.value.id, ev(node.slice))
        if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == "sym"
                and len(node.args) == 1 and isinstance(node.args[0], ast.Constant)):
            return syms.addr(node.args[0].value)
        raise ProbeError("unsupported expression %r" % (expr,))

    return ev(ast.parse(str(expr), mode="eval").body)


def render(expr, variables, syms):
    """Hex text for a command argument; '<expr>' when it depends on an unread value."""
    try:
        return "0x%X" % evaluate(expr, variables, syms)
    except Unresolved:
        return "<%s>" % expr


# ---------------------------------------------------------------- transports
class DaemonTransport:
    """Talks to mlserald through mlserial.cmd() (tools/mlserial/mlserial.py)."""
    name = "daemon"

    def __init__(self):
        sys.path.insert(0, MLSERIAL_DIR)
        import mlserial
        self._ml = mlserial

    def cmd(self, text, timeout):
        try:
            return self._ml.cmd(text, timeout=timeout)
        except SystemExit as e:      # mlserial.die() when the daemon is down
            raise Abort("mlserial daemon not reachable (exit %s); start it: mlserial.py daemon start" % e.code)


class DryRunTransport:
    """No camera: every command 'succeeds' with an empty body; the session treats values as unread."""
    name = "dry-run"

    def cmd(self, text, timeout):
        return {"status": "ok", "body": []}


class ReplayTransport:
    """Canned replies.  File: {"replies": {"<cmd text>": [reply, ...]}, "memory": {"0xADDR": word}}.

    A reply is a list of body lines or {"status": .., "body": [..], "msg": ..}; a list of replies is
    consumed in order and the last one repeats.  rd/md/dump with no canned reply are synthesised from
    "memory" (word values, or lists of values consumed per read; unmapped words read "default", 0).
    """
    name = "replay"

    def __init__(self, data):
        self.replies = {k: list(v) for k, v in data.get("replies", {}).items()}
        self.memory = {int(k, 16): v for k, v in data.get("memory", {}).items()}
        self.default = int(str(data.get("default", 0)), 0)
        self.sent = []

    def _word(self, addr):
        v = self.memory.get(addr, self.default)
        if isinstance(v, list):
            v = v.pop(0) if len(v) > 1 else v[0]
        return int(str(v), 0) & 0xFFFFFFFF

    def _synth(self, text):
        parts = text.split()
        if parts[0] == "ver":
            return ["ML M50mk2 1.0.1 (replay)"]
        if parts[0] in ("rd", "md"):
            addr = int(parts[1], 16) & ~3
            n = int(parts[2], 0) if len(parts) > 2 and parts[0] == "md" else 1
            words = [self._word(addr + 4 * i) for i in range(n)]
            return [format_words(addr + 4 * i, words[i:i + 4]) for i in range(0, n, 4)]
        if parts[0] == "dump":
            addr, length = int(parts[1], 16) & ~3, int(parts[2], 16)
            blob = b"".join(self._word(addr + o).to_bytes(4, "little") for o in range(0, length, 4))[:length]
            rows = ["D 0x%08X %s" % (addr + o, blob[o:o + 32].hex()) for o in range(0, length, 32)]
            return rows + ["=crc 0x%08X" % (zlib.crc32(blob) & 0xFFFFFFFF)]
        return None

    def cmd(self, text, timeout):
        self.sent.append(text)
        queue = self.replies.get(text)
        if queue:
            rep = queue.pop(0) if len(queue) > 1 else queue[0]
            return {"status": "ok", "body": rep} if isinstance(rep, list) else dict({"body": []}, **rep)
        body = self._synth(text)
        if body is None:
            return {"status": "err", "body": [], "msg": "no canned reply for %r" % text}
        return {"status": "ok", "body": body}


def format_words(addr, words):
    return "0x%08X: %s" % (addr, " ".join("0x%08X" % w for w in words))


# ---------------------------------------------------------------- reply parsing
def parse_words(body, addr, n):
    """Parse `rd`/`md` rows; require contiguous addresses starting at addr and exactly n words."""
    words, expect = [], addr & ~3
    for line in body:
        m = WORD_ROW.match(line.strip())
        if not m:
            continue                      # echo, async log fragments, blank lines
        if int(m.group(1), 16) != expect:
            raise ProbeError("row address 0x%s, expected 0x%08X" % (m.group(1), expect))
        row = [int(t, 16) for t in m.group(2).split()]
        words += row
        expect += 4 * len(row)
    if len(words) != n:
        raise ProbeError("got %d words, expected %d (reply shredded?)" % (len(words), n))
    return words


def parse_dump(body, length):
    data, crc = bytearray(), None
    for line in body:
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "D":
            data += bytes.fromhex(parts[2])
        elif parts and parts[0] == "=crc":
            crc = int(parts[1], 16)
    if len(data) != length:
        raise ProbeError("dump returned %d bytes, expected %d" % (len(data), length))
    if crc is None or crc != (zlib.crc32(bytes(data)) & 0xFFFFFFFF):
        raise ProbeError("dump CRC mismatch")
    return bytes(data)


def parse_call(body):
    for line in body:
        m = CALL_RET.match(line.strip())
        if m:
            return int(m.group(1), 16)
    raise ProbeError("no 'ret 0x........' line in call reply")


# ---------------------------------------------------------------- session (command layer)
class Session:
    def __init__(self, transport, syms, opts, sleep=time.sleep, out=sys.stdout):
        self.t, self.syms, self.opts, self.sleep, self.out = transport, syms, opts, sleep, out
        self.dry = isinstance(transport, DryRunTransport)
        self.log = []                     # every command sent, in order
        self._last = 0.0

    def send(self, text, timeout=5.0):
        gap = self.opts.min_gap - (time.time() - self._last)
        if gap > 0 and isinstance(self.t, DaemonTransport):
            self.sleep(gap)
        if self.dry:
            self.out.write("  %s\n" % text)
        res = self.t.cmd(text, timeout)
        self._last = time.time()
        self.log.append({"t": round(self._last, 3), "cmd": text, "status": res.get("status"),
                         "body": res.get("body", []), "msg": res.get("msg", "")})
        return res

    def alive(self):
        try:
            return self.send("ver", 4).get("status") == "ok"
        except Abort:
            return False

    def _idempotent(self, text, timeout, parse):
        """Reads are repeatable: retry on a shredded reply; on silence check the shell is alive."""
        err = None
        for _ in range(1 + READ_RETRIES):
            res = self.send(text, timeout)
            if res.get("status") == "ok":
                try:
                    return parse(res.get("body", []))
                except ProbeError as e:
                    err = e
                    continue
            elif res.get("status") == "timeout":
                if not self.alive():
                    raise Abort("shell stopped answering after %r" % text)
                err = ProbeError("timeout on %r" % text)
            else:
                raise ProbeError("%s -> =err %s" % (text, res.get("msg", "")))
        raise err

    def read_words(self, addr_text, n):
        if n > MD_MAX_WORDS:
            raise ProbeError("md is limited to 0x100 words")
        text = "rd %s" % addr_text if n == 1 else "md %s %d" % (addr_text, n)
        if self.dry:
            self.send(text)
            return None
        return self._idempotent(text, 5 + n / 40.0, lambda b: parse_words(b, int(addr_text, 16), n))

    def dump(self, addr_text, length):
        text = "dump %s 0x%X" % (addr_text, length)
        if self.dry:
            self.send(text)
            return None
        return self._idempotent(text, 10 + length / 1000.0, lambda b: parse_dump(b, length))

    def call(self, addr_text, args, timeout):
        """NOT retried.  A timeout means the callee blocked and the shell task is wedged."""
        text = " ".join(["call", addr_text] + args)
        res = self.send(text, timeout)
        if self.dry:
            return None
        if res.get("status") == "timeout":
            raise Abort("call %s did not return within %ss: shell wedged on a blocking callee "
                        "(cold boot needed)" % (addr_text, timeout))
        if res.get("status") != "ok":
            raise ProbeError("%s -> =err %s" % (text, res.get("msg", "")))
        return [parse_call(res.get("body", []))]

    def write(self, addr_text, value_text):
        res = self.send("wr %s %s" % (addr_text, value_text))
        if not self.dry and res.get("status") != "ok":
            raise ProbeError("wr %s -> %s %s" % (addr_text, res.get("status"), res.get("msg", "")))


# ---------------------------------------------------------------- steps
def step_risk(step, probe):
    if "call" in step:
        return "call"
    if "wr" in step:
        return "wr"
    return step.get("risk") or (probe.get("risk") if "read" in step or "dump" in step else None)


def probe_risks(probe):
    return sorted({r for r in (step_risk(s, probe) for s in probe["steps"]) if r})


def derive(step, variables, syms):
    if step["derive"] == "leading_eq":
        words, value = variables[step["var"]], evaluate(step["value"], variables, syms)
        if words is None:
            return None
        n = 0
        while n < len(words) and words[n] == value:
            n += 1
        return [n]
    if step["derive"] == "freelist":
        pool = variables[step["pool"]]
        if pool is None:
            return None
        return list(walk_freelist(pool, evaluate(step["head"], variables, syms),
                                  evaluate(step["base"], variables, syms), step.get("node", 16)))
    raise ProbeError("unknown derive %r" % step["derive"])


def walk_freelist(pool, head, base, node):
    """Count nodes on a singly linked free list held in a dumped pool.  Returns (count, clean):
    clean = 1 when the list ended at 0; 0 when it left the pool or looped (dump raced the camera)."""
    seen, cur, limit = 0, head, len(pool) // node
    while cur:
        off = cur - base
        if off < 0 or off + 4 > len(pool) or off % node or seen >= limit:
            return seen, 0
        seen += 1
        cur = int.from_bytes(pool[off:off + 4], "little")
    return seen, 1


def run_steps(sess, probe, variables, first):
    """Execute a probe's steps into `variables`.  `first` = False skips steps marked once."""
    syms = sess.syms
    for step in probe["steps"]:
        if step.get("once") and not first:
            continue
        name = step.get("as")
        if "cmd" in step:
            res = sess.send(step["cmd"])
            if not sess.dry and res.get("status") != "ok":
                raise ProbeError("%s -> %s" % (step["cmd"], res.get("status")))
            variables[name] = None if sess.dry else [l for l in res.get("body", []) if l.strip()]
        elif "read" in step:
            variables[name] = sess.read_words(render(step["read"], variables, syms), int(step.get("n", 1)))
        elif "dump" in step:
            variables[name] = sess.dump(render(step["dump"], variables, syms), evaluate(step["len"], variables, syms))
        elif "call" in step:
            args = [render(a, variables, syms) for a in step.get("args", [])]
            variables[name] = sess.call(render(step["call"], variables, syms), args, step.get("timeout", 3))
        elif "wr" in step:
            sess.write(render(step["wr"], variables, syms), render(step["value"], variables, syms))
        elif "derive" in step:
            variables[name] = derive(step, variables, syms)
        elif "sleep" in step:
            if not isinstance(sess.t, ReplayTransport):
                sess.sleep(0 if sess.dry else step["sleep"])
        elif "require" in step:
            if not sess.dry and not evaluate(step["require"], variables, syms):
                raise Skip(step.get("else", "requirement %s not met" % step["require"]))
        else:
            raise ProbeError("unknown step %r" % (step,))


# ---------------------------------------------------------------- checks
def _hexlist(words):
    return ["0x%08X" % w if isinstance(w, int) else w for w in words]


def run_check(chk, variables, syms):
    """Return {"outcome": pass|fail|info, "observed": .., "meaning": ..} for one check."""
    op = chk["op"]
    val = lambda key: evaluate(chk[key], variables, syms)
    words = variables.get(chk.get("var"))
    if op == "info":
        return {"op": op, "var": chk["var"], "outcome": "info", "observed": _hexlist(words), "meaning": ""}
    if op == "any_nonzero":
        got = [variables[v][0] for v in chk["vars"]]
        ok, observed = any(got), _hexlist(got)
    elif op in ("words_eq", "none_eq", "var_eq", "var_ne"):
        observed = _hexlist(words)
        if op == "words_eq":
            ok = words == [evaluate(v, variables, syms) for v in chk["value"]]
        elif op == "none_eq":
            ok = val("value") not in words
        else:
            ok = (words == variables[chk["other"]]) == (op == "var_eq")
    else:
        w = words[chk.get("word", 0)]
        if "mask" in chk:
            w &= val("mask")
        observed = "0x%08X" % w
        if op == "byte_eq":
            b = (w >> (8 * chk["byte"])) & 0xFF
            ok, observed = b == val("value"), "0x%02X" % b
        elif op == "eq":
            ok = w == (val("value") & (val("mask") if "mask" in chk else 0xFFFFFFFF))
        elif op == "ne":
            ok = w != val("value")
        elif op == "lt":
            ok = w < val("value")
        elif op == "nonzero":
            ok = w != 0
        elif op == "in":
            ok = w in [evaluate(v, variables, syms) for v in chk["value"]]
        elif op in ("range", "not_range"):
            ok = (val("lo") <= w <= val("hi")) == (op == "range")
        elif op == "in_sym_range":
            lo, hi = syms.ml_range()
            ok = lo <= (w & ~1) <= hi
            observed += " (%s)" % syms.nearest(w) if ok else ""
        else:
            raise ProbeError("unknown check op %r" % op)
    outcome = "pass" if ok else ("info" if chk.get("severity") == "info" else "fail")
    return {"op": op, "var": chk.get("var", ",".join(chk.get("vars", []))), "word": chk.get("word"),
            "outcome": outcome, "observed": observed, "meaning": chk["pass" if ok else "fail"]}


def verdict_of(checks):
    outcomes = {c["outcome"] for c in checks}
    return "FAIL" if "fail" in outcomes else ("PASS" if "pass" in outcomes else "INFO")


# ---------------------------------------------------------------- runner
def probes_for(doc, stage):
    return [p for p in doc["probes"] if stage in p["stages"]]


def gated(probe, opts):
    """Risks of this probe that the command line has not enabled."""
    return [r for r in probe_risks(probe) if not getattr(opts, RISK_FLAGS[r])]


def base_result(probe, stage):
    return {"id": probe["id"], "stage": stage, "source": probe["source"], "state": probe["state"],
            "title": probe["title"], "decision": probe["decision"]}


def run_once(sess, probe, stage, opts):
    res = base_result(probe, stage)
    blocked = gated(probe, opts)
    if blocked:
        return dict(res, verdict="SKIPPED", reason="needs --%s" % ", --".join(RISK_FLAGS[r].replace("_", "-") for r in blocked))
    variables = {}
    try:
        run_steps(sess, probe, variables, True)
        if sess.dry:
            return dict(res, verdict="DRY")
        checks = [run_check(c, variables, sess.syms) for c in probe.get("checks", [])]
        res.update(verdict=verdict_of(checks), checks=checks)
    except Skip as e:
        res.update(verdict="SKIPPED", reason=str(e))
    except (ProbeError, KeyError, IndexError, TypeError) as e:
        res.update(verdict="ERROR", reason="%s: %s" % (type(e).__name__, e))
    res["values"] = {k: _hexlist(v) for k, v in variables.items() if v is not None and len(v) <= 64}
    return res


def run_sampled(sess, probes, stage, opts, count, interval):
    """Sampling loop: every probe once per round, `interval` seconds between rounds."""
    states = []
    for p in probes:
        blocked = gated(p, opts)
        st = {"probe": p, "res": base_result(p, stage), "vars": {}, "samples": [], "tally": {}, "dead": None}
        if blocked:
            st["dead"] = ("SKIPPED", "needs --%s" % ", --".join(RISK_FLAGS[r].replace("_", "-") for r in blocked))
        states.append(st)
    for i in range(count):
        t0 = time.time()
        for st in states:
            if st["dead"]:
                continue
            try:
                run_steps(sess, st["probe"], st["vars"], i == 0)
                if sess.dry:
                    continue
                checks = [run_check(c, st["vars"], sess.syms) for c in st["probe"].get("checks", [])]
            except Skip as e:
                st["dead"] = ("SKIPPED", str(e))
                continue
            except (ProbeError, KeyError, IndexError, TypeError) as e:
                st["dead"] = ("ERROR", "sample %d: %s" % (i, e))
                continue
            once = {s.get("as") for s in st["probe"]["steps"] if s.get("once")}
            st["samples"].append({"t": round(time.time(), 3),
                                  "values": {k: _hexlist(v) for k, v in st["vars"].items() if k not in once}})
            for n, c in enumerate(checks):
                t = st["tally"].setdefault(n, dict(c, n_pass=0, n_fail=0, n_info=0))
                t["n_" + c["outcome"]] += 1
                if c["outcome"] == "fail" or t["outcome"] != "fail":
                    t.update(outcome=c["outcome"], observed=c["observed"], meaning=c["meaning"])
        if sess.dry:
            sess.out.write("  # ... this sampling round repeats %d times, %.2f s apart "
                           "(steps marked once only in the first round)\n" % (count, interval))
            break
        if i + 1 < count and isinstance(sess.t, DaemonTransport):
            sess.sleep(max(0.0, interval - (time.time() - t0)))
    return [finish_sampled(st, sess.dry) for st in states]


def finish_sampled(st, dry):
    res = st["res"]
    if st["dead"] and not st["samples"]:
        return dict(res, verdict=st["dead"][0], reason=st["dead"][1])
    if dry:
        return dict(res, verdict="DRY")
    checks = [st["tally"][k] for k in sorted(st["tally"])]
    distinct = {}
    for s in st["samples"]:
        for k, v in s["values"].items():
            d = distinct.setdefault(k, [])
            if v not in d and len(d) < 16:
                d.append(v)
    res.update(verdict=verdict_of(checks), checks=checks, n_samples=len(st["samples"]),
               distinct=distinct, samples=st["samples"])
    if st["dead"]:
        res.update(verdict="ERROR", reason=st["dead"][1])
    return res


def tally_log(path, patterns, t0, t1, tail_bytes=16 << 20):
    """Count assert/abort lines the daemon logged between t0 and t1 (read-only pass over uart.jsonl)."""
    counts = {k: 0 for k in patterns if not k.startswith("_")}
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - tail_bytes))
            lines = f.read().decode("utf-8", "replace").splitlines()
    except OSError as e:
        return {"error": str(e)}
    for line in lines:
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if rec.get("dir") == "rx" and t0 <= rec.get("ts", 0) <= t1:
            for k in counts:
                counts[k] += patterns[k] in rec.get("text", "")
    return counts


def print_plan(doc, opts, stages, out):
    out.write("PLAN (stages %s). Reads of plain RAM run by default; everything else is listed here.\n"
              % ",".join(map(str, stages)))
    for stage in stages:
        out.write("\nStage %d: %s\n" % (stage, doc["stages"][str(stage)]))
        for p in probes_for(doc, stage):
            risks, blocked = probe_risks(p), gated(p, opts)
            mark = "SKIP (needs --%s)" % ", --".join(RISK_FLAGS[r].replace("_", "-") for r in blocked) if blocked else "run"
            out.write("  %-12s %-9s %-4s %s%s\n" % (p["id"], p["state"], "xN" if p.get("sample") else "x1",
                                                   p["title"], "  [%s: %s]" % ("+".join(risks), mark) if risks else ""))
            for s in p["steps"]:
                if "call" in s or "wr" in s:
                    out.write("      %s %s %s\n" % ("CALL" if "call" in s else "WRITE", s.get("call", s.get("wr")),
                                                   " ".join(map(str, s.get("args", [s.get("value", "")])))))
    if 2 in stages:
        er = doc["enable_raw"]
        if opts.allow_call and opts.enable_raw_by_call:
            for c in er["calls"]:
                out.write("\n  Stage 2 will CALL %s (%s)\n" % (c["call"], c["note"]))
        else:
            out.write("\n  Stage 2 asks the operator: %s\n" % er["operator_text"])
    out.write("\n")


def operator(opts, sess, text):
    if sess.dry or isinstance(sess.t, ReplayTransport) or opts.yes:
        sess.out.write("  # operator: %s\n" % text)
        return
    input("\n>>> %s\n    Press Enter when done (Ctrl-C aborts the run)... " % text)


def enable_raw(doc, sess, opts):
    er = doc["enable_raw"]
    if not (opts.allow_call and opts.enable_raw_by_call):
        operator(opts, sess, er["operator_text"])
        return {"how": "operator"}
    for c in er["calls"]:
        ret = sess.call(render(c["call"], {}, sess.syms), [render(a, {}, sess.syms) for a in c.get("args", [])], c["timeout"])
    if not sess.dry and not isinstance(sess.t, ReplayTransport):
        sess.sleep(0.5)
    return {"how": "call", "ret": _hexlist(ret) if ret else None}


def run_stage(doc, sess, opts, stage, run):
    probes = probes_for(doc, stage)
    single = [p for p in probes if not p.get("sample")]
    sampled = [p for p in probes if p.get("sample")]
    single.sort(key=lambda p: bool(probe_risks(p)))      # plain RAM first: a hang then loses the least
    if sess.dry:
        sess.out.write("\n# stage %d: %s\n" % (stage, doc["stages"][str(stage)]))
    if stage == 2:
        run["raw_enable"] = enable_raw(doc, sess, opts)
    if stage == 3:
        operator(opts, sess, "Start the recording attempt NOW; sampling runs for %.0f s." % opts.duration)
    interval = max(MIN_SAMPLE_INTERVAL, opts.interval)
    count = max(1, math.ceil(opts.duration / interval)) if stage == 3 else opts.samples
    t0 = time.time()
    if stage == 3:                                        # time-critical: sample first, one-shots after
        run["results"] += run_sampled(sess, sampled, stage, opts, count, interval)
        run["log_tally"] = {"t0": t0, "t1": time.time(), "counts": {} if sess.dry or isinstance(sess.t, ReplayTransport)
                            else tally_log(opts.uart_log, doc["log_patterns"], t0, time.time() + 1)}
    for p in single:
        run["results"].append(run_once(sess, p, stage, opts))
    if stage != 3 and sampled:
        run["results"] += run_sampled(sess, sampled, stage, opts, count, interval)


def execute(doc, sess, opts, stages):
    run = {"tool": "hwprobe", "started": datetime.datetime.now().isoformat(timespec="seconds"),
           "transport": sess.t.name, "label": opts.label, "stages": stages,
           "sym": {"path": os.path.abspath(sess.syms.path), "sha1": sess.syms.sha1()},
           "flags": {f: bool(getattr(opts, f)) for f in RISK_FLAGS.values()},
           "caveat": "Expected values are static-analysis predictions; a PASS confirms the prediction "
                     "for this boot and camera state only.",
           "results": [], "aborted": None}
    try:
        if not sess.dry and not sess.alive():
            raise Abort("shell does not answer `ver`")
        for stage in stages:
            run_stage(doc, sess, opts, stage, run)
    except Abort as e:
        run["aborted"] = str(e)
    except KeyboardInterrupt:
        run["aborted"] = "interrupted by operator"
    run["commands"] = sess.log
    return run


# ---------------------------------------------------------------- reports
def markdown(run):
    lines = ["# hwprobe run %s%s" % (run["started"], " (%s)" % run["label"] if run["label"] else ""), "",
             "- transport: %s; stages: %s" % (run["transport"], ",".join(map(str, run["stages"]))),
             "- sym: `%s` (sha1 %s)" % (run["sym"]["path"], run["sym"]["sha1"][:12]),
             "- flags: %s" % ", ".join("%s=%s" % kv for kv in run["flags"].items()),
             "- %s" % run["caveat"]]
    if run["aborted"]:
        lines.append("- **RUN ABORTED: %s** (results below are partial)" % run["aborted"])
    counts = {}
    for r in run["results"]:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    lines += ["- verdicts: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())), "",
              "| stage | probe | verdict | observed / meaning | decision | source |", "|---|---|---|---|---|---|"]
    for r in run["results"]:
        cells = []
        for c in r.get("checks", []):
            n = " [%d pass / %d fail of %d]" % (c["n_pass"], c["n_fail"], r["n_samples"]) if "n_pass" in c else ""
            obs = c["observed"] if isinstance(c["observed"], str) else " ".join(c["observed"] or [])
            cells.append("%s `%s`%s: %s %s" % (c["var"], obs[:120], n, c["outcome"].upper(), c["meaning"]))
        if r.get("reason"):
            cells.append(r["reason"])
        lines.append("| %d | %s %s | **%s** | %s | %s | %s |" % (
            r["stage"], r["id"], r["title"], r["verdict"], "<br>".join(cells).replace("|", "/"), r["decision"], r["source"]))
    if run.get("log_tally"):
        lines += ["", "UART assert tally during stage 3: `%s`" % json.dumps(run["log_tally"]["counts"])]
    for cf in run.get("card_files", []):
        lines += ["", "## Card file %s" % cf["file"], "", "- checker: %s, exit status %s" % (cf["checker"], cf["exit"]),
                  "", "```", cf["output"][:4000], "```"]
    return "\n".join(lines) + "\n"


def write_reports(run, out_dir, stamp=None):
    os.makedirs(out_dir, exist_ok=True)
    stamp = stamp or datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    base = os.path.join(out_dir, "hwprobe-%s%s" % (stamp, "-" + re.sub(r"[^\w.-]", "_", run["label"]) if run["label"] else ""))
    with open(base + ".json", "w") as f:
        json.dump(run, f, indent=1)
    with open(base + ".md", "w") as f:
        f.write(markdown(run))
    return base + ".json", base + ".md"


# ---------------------------------------------------------------- after the session
def check_card_file(path, mlvcheck_dir):
    """Run mlv_check.py (.MLV) or rvi_check.py (.RVI, if the checker exists) on one file."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".rvi":
        tool, extra = os.path.join(mlvcheck_dir, "rvi_check.py"), []
        mlv = os.path.splitext(path)[0] + ".MLV"
        extra = ["--mlv", mlv] if os.path.exists(mlv) else []
    else:
        tool, extra = os.path.join(mlvcheck_dir, "mlv_check.py"), ["--crash-ok", "--stats"]
    if not os.path.exists(tool):
        return {"file": path, "checker": os.path.basename(tool), "exit": None, "output": "checker not present"}
    p = subprocess.run([sys.executable, tool] + extra + [path], capture_output=True, text=True, timeout=600)
    return {"file": os.path.abspath(path), "checker": os.path.basename(tool), "exit": p.returncode,
            "output": (p.stdout + p.stderr).strip()}


def after_session(result_json, files, mlvcheck_dir):
    with open(result_json) as f:
        run = json.load(f)
    run.setdefault("card_files", [])
    run["card_files"] += [check_card_file(p, mlvcheck_dir) for p in files]
    with open(result_json, "w") as f:
        json.dump(run, f, indent=1)
    with open(os.path.splitext(result_json)[0] + ".md", "w") as f:
        f.write(markdown(run))
    return run


# ---------------------------------------------------------------- CLI
def build_parser():
    ap = argparse.ArgumentParser(prog="hwprobe", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="op", required=True)
    for name in ("plan", "run"):
        p = sub.add_parser(name)
        p.add_argument("--sym", required=True, help=".sym of the build that is RUNNING on the camera")
        p.add_argument("--probes", default=DEFAULT_PROBES)
        p.add_argument("--stages", default="0,1", help="comma list of 0,1,2,3 (default 0,1)")
        p.add_argument("--only", default="", help="comma list of probe ids")
        p.add_argument("--allow-call", action="store_true", help="permit `call` steps (can wedge the shell)")
        p.add_argument("--allow-wr", action="store_true", help="permit `wr` steps")
        p.add_argument("--allow-risky-read", action="store_true", help="permit MMIO / page-table reads (LiveView only)")
        p.add_argument("--enable-raw-by-call", action="store_true", help="stage 2: enable raw with a call (needs --allow-call)")
        p.add_argument("--samples", type=int, default=10, help="rounds in stage 1/2 sampling")
        p.add_argument("--interval", type=float, default=0.5, help="seconds between rounds (floor 0.2)")
        p.add_argument("--duration", type=float, default=20.0, help="stage 3 sampling time, seconds")
        p.add_argument("--min-gap", type=float, default=0.05, help="minimum seconds between commands")
        p.add_argument("--label", default="", help="camera state for this run, e.g. photo-lv, 1080p25")
        p.add_argument("--yes", action="store_true", help="do not wait for the operator at stage 2/3")
        p.add_argument("--out", default=DEFAULT_OUT)
        p.add_argument("--uart-log", default=DEFAULT_UARTLOG)
        if name == "run":
            p.add_argument("--dry-run", action="store_true", help="print the command sequence, no camera")
            p.add_argument("--replay", help="JSON file with canned replies (see ReplayTransport)")
    p = sub.add_parser("after", help="check files copied from the card and append to a result")
    p.add_argument("--result", required=True)
    p.add_argument("--mlvcheck-dir", default=os.path.join(TOOLS, "mlvcheck"))
    p.add_argument("files", nargs="+")
    return ap


def main(argv=None, out=sys.stdout):
    opts = build_parser().parse_args(argv)
    if opts.op == "after":
        run = after_session(opts.result, opts.files, opts.mlvcheck_dir)
        bad = [c for c in run["card_files"] if c["exit"]]
        out.write("%d file(s) checked, %d with non-zero status; appended to %s\n" % (len(opts.files), len(bad), opts.result))
        return 1 if bad else 0
    with open(opts.probes) as f:
        doc = json.load(f)
    stages = sorted({int(s) for s in opts.stages.split(",") if s.strip()})
    if opts.only:
        keep = set(opts.only.split(","))
        doc["probes"] = [p for p in doc["probes"] if p["id"] in keep]
    syms = SymTable(opts.sym)
    print_plan(doc, opts, stages, out)
    if opts.op == "plan":
        return 0
    if opts.dry_run:
        transport = DryRunTransport()
    elif opts.replay:
        with open(opts.replay) as f:
            transport = ReplayTransport(json.load(f))
    else:
        transport = DaemonTransport()
    sess = Session(transport, syms, opts, out=out)
    run = execute(doc, sess, opts, stages)
    if opts.dry_run:
        out.write("\n%d commands; nothing was sent.\n" % len(sess.log))
        return 0
    js, md = write_reports(run, opts.out)
    out.write(markdown(run).split("\n| stage")[0] + "\nresult: %s\n        %s\n" % (js, md))
    if run["aborted"]:
        return 3
    return 1 if any(r["verdict"] in ("FAIL", "ERROR") for r in run["results"]) else 0


if __name__ == "__main__":
    sys.exit(main())
