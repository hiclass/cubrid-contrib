#!/usr/bin/env python3
"""Fuzzer for cub_ctv --replay: damaged records must be refused or replayed,
never crash, hang or run into undefined behaviour.

Each case is a seed record (a valid CTVREC 5 file) with a few mutations:
boundary values in tokens, lines deleted / repeated / moved / mixed across
records, token counts changed, junk lines, truncation.  The tool replays it
in one view (scrolling, --json, --detail, --trans, --narrow, --wide, --plain;
with --pty-share also the interactive screen on a pty).  Case i depends only
on (--seed, i) and the seed records, so a run is repeatable.

A case fails when the tool is killed by a signal, runs past --tmo, ends with
an exit code other than 0 / 1, or (sanitizer build) prints a sanitizer
report.  A signed-overflow report whose first frame is a function listed
"open" in the MANIFEST of --fixed is known and counted, not a failure.
The tool runs die with this program (SIGTERM, SIGINT and a parent's death
included): none is left behind.

--fixed DIR replays the kept inputs of DIR/MANIFEST first, one row each:
    <file> <view> <state> <function>
view: scroll, json, detail, trans, narrow, wide, or pty:ROWSxCOLS:KEYS.
state "open": undefined behaviour the reader does not refuse yet; a sanitizer
build must report a signed overflow in <function> (a row that stops
reporting fails, so it is moved to "fixed").  state "fixed": no report in any build.  Without a
sanitizer build both only have to end with exit 0 / 1.

usage: fuzz_replay.py --tool BIN --seeds DIR_OR_FILE... [--fixed DIR]
           [--n N] [--seed S] [--jobs J] [--tmo SEC] [--pty-share F]
           [--keep DIR]
exit 0: no failure; 1: failures (reproducers under --keep, or a kept
temporary directory whose name is printed).
"""
import argparse, ctypes, os, re, random, sys, signal, select, struct
import subprocess, tempfile, shutil
from multiprocessing import Pool

BOUND = ["0", "-1", "1", "2", "2147483647", "-2147483648", "2147483648",
         "-2147483649", "4294967295", "4294967296", "4294967297",
         "-4294967296", "9223372036854775807", "-9223372036854775808",
         "9223372036854775808", "18446744073709551615", "18446744073709551616",
         "1e308", "-1e308", "1.8e308", "1e-308", "4.9e-324", "-4.9e-324",
         "nan", "-nan", "inf", "-inf", "infinity", "0x7fffffff", "+28", "028",
         "28x", "x", "-", "~", "%", "%0", "%00", "%zz", "%7e%25", "%ff%fe",
         "%c3%28", "%e2%80%ae", "%1b[2J", "1" * 400, "9" * 40, "-0", "0.0",
         "1e7", "1.0000001e7", "10000", "10001", "3600", "3600.0001", "0.1",
         "0.09999", "9", "10", "27", "28", "29", "63", "64", "65", "66", "127",
         "128", "4095", "4096", "8191", "8192", "65535", "65536",
         "36010000", "36010001", "1023", "1024",
         "1073741823", "1073741824", "-1073741824", "-1073741825", "-2147483647"]
LONGS = ["A" * 70, "%41" * 70, "B" * 5000, "%e2%80%ae" * 30, "é" * 40]
VIEWS = {"scroll": [], "json": ["--json"], "detail": ["--detail"],
         "trans": ["--trans"], "narrow": ["--narrow"], "wide": ["--wide"],
         "plain": ["--plain"], "trans-json": ["--trans", "--json"],
         "detail-plain": ["--detail", "--plain"]}
PTY_SIZES = [(1, 1), (2, 10), (5, 20), (24, 80), (50, 140), (120, 300)]
PTY_KEYS = ["++++++q", "++++++<<>>..,,q", " .,.,lq", "++++++R+q", "<>" * 6 + "q",
            "++++++" + "." * 20 + "q", "l+++++++q"]


def load_seeds(paths):
    """valid records only: the files directly under each directory"""
    names = []
    for p in paths:
        if os.path.isdir(p):
            names += [os.path.join(p, f) for f in os.listdir(p) if f.endswith(".rec")]
        else:
            names.append(p)
    out = []
    for s in sorted(set(names), key=os.path.basename):
        b = open(s, "rb").read()
        if b.startswith(b"CTVREC 5\n"):
            out.append((os.path.basename(s), b.split(b"\n")))
    return out


def frames_of(lines):
    """indices of F..E spans"""
    spans, st = [], None
    for i, l in enumerate(lines):
        if l.startswith(b"F "):
            st = i
        elif l == b"E" and st is not None:
            spans.append((st, i))
            st = None
    return spans


def crop(rng, lines, maxf=4):
    """keep magic, header and a few whole frames (speed)"""
    sp = frames_of(lines)
    if len(sp) <= maxf:
        return lines
    a = rng.randrange(0, len(sp) - maxf + 1)
    out = lines[:2]
    for s, e in sp[a:a + maxf]:
        out += lines[s:e + 1]
    return out + [b""]


def tok_mut(rng, line):
    t = line.split(b" ")
    if len(t) < 2:
        return line
    i = rng.randrange(1, len(t)) if rng.random() < 0.97 else 0
    cur = t[i].decode("latin-1")
    r = rng.random()
    if r < 0.55:
        v = rng.choice(BOUND)
    elif r < 0.75 and re.fullmatch(r"-?\d+", cur):
        n = int(cur)
        v = str(rng.choice([n + 1, n - 1, n * 2, -n, n + 2**31, n - 2**32, n * 1000003]))
    elif r < 0.85:
        v = rng.choice(LONGS)
    elif r < 0.92:
        v = cur + rng.choice(["x", "\r", "%", "~", "0", ".5", "e400"])
    else:
        v = "" if rng.random() < 0.5 else "  "
    t[i] = v.encode("utf-8")
    return b" ".join(t)


def chain_mut(rng, line):
    """Q / P / V with a consistent count and boundary slots"""
    t = line.split(b" ")
    tag = t[0]
    slots = [b"0", b"-1", b"65535", b"65536", b"2147483647", b"-2147483648",
             b"4227", b"4300", b"8191", b"1"]
    if tag == b"Q":
        n = rng.choice([0, 1, 63, 64, 65, rng.randrange(0, 66)])
        return b" ".join([b"Q", t[1] if len(t) > 1 else b"1", str(n).encode()]
                         + [rng.choice(slots) for _ in range(n)])
    if tag == b"P":
        n = rng.choice([0, 1, 64, rng.randrange(0, 65)])
        cs = rng.choice([b"0", b"1", b"-1", b"63", b"64", t[1] if len(t) > 1 else b"0"])
        return b" ".join([b"P", cs, str(n).encode()] + [rng.choice(slots) for _ in range(n)])
    if tag == b"V":
        n = rng.choice([0, 1, 9, rng.randrange(0, 10)])
        out = [b"V", str(n).encode()]
        for _ in range(n):
            out += [rng.choice([b"1", b"9", b"0", b"10", b"5"]),
                    rng.choice([b"-1", b"0", b"1", b"17", b"63", b"64", b"-2"])]
        return b" ".join(out)
    return line


def mutate(rng, seeds, base):
    lines = list(base)
    log = []
    for _ in range(rng.choice([1, 1, 1, 2, 2, 3, 4, 6])):
        body = [i for i in range(1, len(lines)) if lines[i]]
        if not body:
            break
        op = rng.random()
        i = rng.choice(body)
        if op < 0.45:
            lines[i] = tok_mut(rng, lines[i])
            log.append("tok@%d" % (i + 1))
        elif op < 0.52:
            cand = [k for k in body if lines[k][:2] in (b"Q ", b"P ", b"V ")]
            if cand:
                k = rng.choice(cand)
                lines[k] = chain_mut(rng, lines[k])
                log.append("chain@%d" % (k + 1))
        elif op < 0.555:
            # a big count on a summed line, then the line many times
            cand = [k for k in body if lines[k][:2] in (b"W ", b"R ", b"G ", b"O ", b"C ")]
            if cand:
                k = rng.choice(cand)
                t = lines[k].split(b" ")
                j = rng.randrange(1, len(t))
                t[j] = rng.choice([b"2147483647", b"1073741824", b"36010000", b"-2147483648"])
                lines[k:k + 1] = [b" ".join(t)] * rng.choice([2, 3, 70, 130])
                log.append("bigrep@%d.%d" % (k + 1, j))
        elif op < 0.58:
            del lines[i]
            log.append("del@%d" % (i + 1))
        elif op < 0.64:
            lines.insert(rng.choice(body), lines[i])
            log.append("dup@%d" % (i + 1))
        elif op < 0.69:
            j = rng.choice(body)
            lines[i], lines[j] = lines[j], lines[i]
            log.append("swap@%d,%d" % (i + 1, j + 1))
        elif op < 0.75:
            # one line many times (the reader's caps: W 4096, S 8192, T/Q/O 128, G 1024)
            k = rng.choice([130, 1100, 4200, 8300])
            lines[i:i + 1] = [lines[i]] * k
            log.append("rep%d@%d" % (k, i + 1))
        elif op < 0.81:
            # a line of another record into this frame
            _, other = rng.choice(seeds)
            cand = [l for l in other[2:] if l and l[:1] not in (b"F", b"E")]
            if cand:
                lines.insert(rng.choice(body), rng.choice(cand))
                log.append("mix@%d" % (i + 1))
        elif op < 0.85:
            t = lines[i].split(b" ")
            if rng.random() < 0.5 and len(t) > 1:
                t.pop(rng.randrange(1, len(t)))
            else:
                t.insert(rng.randrange(1, len(t) + 1), rng.choice(BOUND).encode())
            lines[i] = b" ".join(t)
            log.append("ntok@%d" % (i + 1))
        elif op < 0.88:
            junk = rng.choice([b"", b"\r", b"\x00", b"F", b"E", b"K 2", b"Z 1",
                               b"X" * 20000, b"I " + b"a" * 17000,
                               b"\x00E", b"E\x00junk", b"\tE"])
            lines.insert(i, junk)
            log.append("junk@%d" % (i + 1))
        elif op < 0.91:
            sp = frames_of(lines)
            if sp:
                s, e = rng.choice(sp)
                k = rng.choice([2, 70, 300])
                lines[e + 1:e + 1] = lines[s:e + 1] * k
                log.append("frames*%d" % k)
        elif op < 0.93:
            lines = [l.replace(b"K 1", b"K 0") for l in lines]
            log.append("allK0")
        elif op < 0.95:
            lines = [l + b"\r" for l in lines]
            log.append("crlf")
        elif op < 0.98:
            lines[1] = tok_mut(rng, lines[1])
            log.append("hdr")
        else:
            j = rng.choice(body)
            ln = lines.pop(i)
            lines.insert(j, ln)
            log.append("move@%d->%d" % (i + 1, j + 1))
    data = b"\n".join(lines)
    if rng.random() < 0.06 and len(data) > 20:
        data = data[:rng.randrange(10, len(data))]
        log.append("trunc")
    return data, log


SAN_EXIT = 97		# a sanitizer's exit code: never taken for a refusal


def report_of(err):
    """(kind, function of the first frame) of a sanitizer report, or None;
    kind: overflow (UBSan signed integer overflow), ubsan (any other), asan"""
    m = re.search(r"runtime error: ([^\n]*)", err)
    if m:
        kind = "overflow" if m.group(1).startswith("signed integer overflow") else "ubsan"
    elif "ERROR: AddressSanitizer" in err or "ERROR: LeakSanitizer" in err:
        kind = "asan"
    else:
        return None
    m = re.search(r"#0 0x[0-9a-f]+ in (\S+)", err)
    return kind, m.group(1) if m else "?"


def run_env():
    return dict(os.environ, LANG="en_US.UTF-8", TERM="xterm",
                ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:exitcode=%d" % SAN_EXIT,
                UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:exitcode=%d" % SAN_EXIT)


def die_with_parent():
    """in the child, before exec: SIGKILL when the process that started it
    dies (PR_SET_PDEATHSIG), so no tool run outlives this program"""
    ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL, 0, 0, 0)


def child_setup():
    os.setsid()
    die_with_parent()


def run_pipe(tool, rec, args, tmo):
    """-> (exit code or None, stderr, timed out, signal)"""
    p = subprocess.Popen([tool, "--replay", rec, "--replay-speed", "0"] + args,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, env=run_env(), preexec_fn=child_setup)
    try:
        _, err = p.communicate(timeout=tmo)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        _, err = p.communicate()
        return None, err.decode("utf-8", "replace"), True, 0
    rc = p.returncode
    return (rc if rc >= 0 else None), err.decode("utf-8", "replace"), False, (-rc if rc < 0 else 0)


def run_pty(tool, rec, rows, cols, keys, tmo, errf):
    """the interactive replay on a pty of rows x cols, keys typed one by one"""
    import pty, fcntl, termios, time
    pid, fd = pty.fork()
    if pid == 0:
        e = os.open(errf, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        os.dup2(e, 2)
        die_with_parent()
        env = run_env()
        env.pop("COLUMNS", None)
        os.execve(tool, ["cub_ctv", "--replay", rec], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    t0 = time.monotonic()

    def pump(sec):
        end = time.monotonic() + sec
        while time.monotonic() < end:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    if not os.read(fd, 1 << 16):
                        return False
                except OSError:
                    return False
        return True
    pump(0.3)
    for k in keys:
        try:
            if k == "R":
                fcntl.ioctl(fd, termios.TIOCSWINSZ,
                            struct.pack("HHHH", max(rows // 2, 1), max(cols // 2, 1), 0, 0))
                os.kill(pid, signal.SIGWINCH)
            else:
                os.write(fd, k.encode())
        except OSError:
            break
        if not pump(0.03):
            break
    status = None
    while time.monotonic() - t0 < tmo:
        w, st = os.waitpid(pid, os.WNOHANG)
        if w == pid:
            status = st
            break
        pump(0.03)
    timed = status is None
    if timed:
        try:
            os.killpg(pid, signal.SIGKILL)
        except OSError:
            pass
        _, status = os.waitpid(pid, 0)
    os.close(fd)
    err = open(errf, "r", errors="replace").read()
    os.unlink(errf)
    if timed:
        return None, err, True, 0
    if os.WIFSIGNALED(status):
        return None, err, False, os.WTERMSIG(status)
    return os.WEXITSTATUS(status), err, False, 0


def run_view(tool, rec, view, tmo):
    if view.startswith("pty:"):
        _, size, keys = view.split(":", 2)
        rows, cols = (int(x) for x in size.split("x"))
        return run_pty(tool, rec, rows, cols, keys, tmo + 4, rec + ".err")
    return run_pipe(tool, rec, VIEWS[view], tmo)


def verdict(rc, err, timed, sig, known):
    """-> (class, detail): ok, known, or a failure class"""
    if timed:
        return "timeout", ""
    rep = report_of(err)
    if rep:
        if rep[0] == "overflow" and rep[1] in known:
            return "known", rep[1]
        return rep[0], rep[1]
    if sig:
        return "signal", str(sig)
    if rc not in (0, 1):
        return "exit", str(rc)
    return "ok", "replayed" if rc == 0 else "refused"


G = {}


def init(cfg, seeds):
    G["cfg"] = cfg
    G["seeds"] = seeds
    # a worker dies at once on the signals the main process cleans up on,
    # and with the main process when that one is killed
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, signal.SIG_DFL)
    ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGTERM, 0, 0, 0)


def one(idx):
    cfg, seeds = G["cfg"], G["seeds"]
    rng = random.Random(cfg["seed"] * 1000003 + idx)
    name, base = rng.choice(seeds)
    data, log = mutate(rng, seeds, crop(rng, base))
    rec = os.path.join(cfg["work"], "c%d.rec" % idx)
    with open(rec, "wb") as f:
        f.write(data)
    if rng.random() < cfg["pty_share"]:
        rows, cols = rng.choice(PTY_SIZES)
        view = "pty:%dx%d:%s" % (rows, cols, rng.choice(PTY_KEYS))
    else:
        view = rng.choice(sorted(VIEWS))
    rc, err, timed, sig = run_view(cfg["tool"], rec, view, cfg["tmo"])
    cls, det = verdict(rc, err, timed, sig, cfg["known"])
    res = {"idx": idx, "seed": name, "view": view, "cls": cls, "det": det, "log": log}
    if cls in ("ok", "known"):
        os.unlink(rec)
    else:
        res["err"] = err[-3000:]
        res["rec"] = rec
    return res


def read_manifest(d):
    rows = []
    for ln in open(os.path.join(d, "MANIFEST")):
        ln = ln.split("#")[0].split()
        if not ln:
            continue
        if len(ln) != 4 or ln[2] not in ("open", "fixed"):
            sys.exit("fuzz FAIL: MANIFEST row: %s" % " ".join(ln))
        rows.append(ln)
    if not rows:
        sys.exit("fuzz FAIL: MANIFEST lists no input")
    listed = set(r[0] for r in rows)
    for f in sorted(os.listdir(d)):
        if f.endswith(".rec") and f not in listed:
            sys.exit("fuzz FAIL: %s has no MANIFEST row" % f)
    return rows


def run_fixed(tool, d, rows, san, tmo, work):
    """-> list of failure texts"""
    bad = []
    for name, view, state, func in rows:
        # a copy: the pty run leaves its stderr file beside the record
        rec = os.path.join(work, name)
        shutil.copy(os.path.join(d, name), rec)
        rc, err, timed, sig = run_view(tool, rec, view, tmo)
        rep = report_of(err)
        what = None
        if timed:
            what = "timeout"
        elif rep and rep[0] != "overflow":
            what = "%s report in %s" % rep
        elif san and state == "open":
            if not rep:
                what = "listed open (%s) but no report: move the row to fixed" % func
            elif rep[1] != func:
                what = "report in %s, listed %s" % (rep[1], func)
        elif rep:
            what = "report in %s, listed fixed" % rep[1]
        elif sig:
            what = "signal %d" % sig
        elif rc not in (0, 1):
            what = "exit %s" % rc
        if what:
            bad.append("%s (%s): %s" % (name, view, what))
        os.unlink(rec)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--seeds", nargs="+", required=True)
    ap.add_argument("--fixed")
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=20261123)
    ap.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--tmo", type=float, default=20)
    ap.add_argument("--pty-share", type=float, default=0)
    ap.add_argument("--keep")
    a = ap.parse_args()
    tool = os.path.abspath(a.tool)
    if not os.access(tool, os.X_OK):
        sys.exit("fuzz FAIL: no tool at %s" % tool)
    seeds = load_seeds(a.seeds)
    if not seeds:
        sys.exit("fuzz FAIL: no seed record")
    # a sanitizer build carries the runtime's handler names
    san = b"__ubsan_handle" in open(tool, "rb").read()
    rows = read_manifest(a.fixed) if a.fixed else []
    known = set(r[3] for r in rows if r[2] == "open")
    keep = a.keep or tempfile.mkdtemp(prefix="cub_ctv_fuzz.")
    os.makedirs(keep, exist_ok=True)
    work = tempfile.mkdtemp(prefix="work.", dir=keep)

    def stopped(sig, frame):
        # one stop: a second signal must not cut the clean-up short
        for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
            signal.signal(s, signal.SIG_IGN)
        signal.alarm(60)    # a clean-up that hangs still ends
        raise KeyboardInterrupt
    for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(s, stopped)
    try:
        run_all(a, tool, seeds, rows, known, san, keep, work)
    except KeyboardInterrupt:
        # the pool is gone (its workers' tool runs die with them)
        shutil.rmtree(work, ignore_errors=True)
        if not a.keep:
            shutil.rmtree(keep, ignore_errors=True)
        print("fuzz FAIL: stopped by a signal")
        sys.exit(1)


def run_all(a, tool, seeds, rows, known, san, keep, work):
    bad = run_fixed(tool, a.fixed, rows, san, a.tmo, work) if rows else []
    for b in bad:
        print("fuzz FAIL: kept input %s" % b)
    cfg = {"tool": tool, "seed": a.seed, "work": work, "tmo": a.tmo,
           "pty_share": a.pty_share, "known": known}
    counts, kn, finds = {}, {}, {}
    with Pool(max(1, min(a.jobs, 8)), initializer=init, initargs=(cfg, seeds)) as pool:
        for r in pool.imap_unordered(one, range(a.n), chunksize=4):
            c = r["cls"]
            if c == "ok":
                counts[r["det"]] = counts.get(r["det"], 0) + 1
            elif c == "known":
                kn[r["det"]] = kn.get(r["det"], 0) + 1
            else:
                k = "%s %s" % (c, r["det"])
                if k not in finds:
                    finds[k] = 0
                    tag = "case%d_%s" % (r["idx"], re.sub(r"[^\w.]+", "_", k).strip("_")[:60])
                    shutil.copy(r["rec"], os.path.join(keep, tag + ".rec"))
                    with open(os.path.join(keep, tag + ".txt"), "w") as f:
                        f.write("view: %s\nseed record: %s\nmutations: %s\n\n%s" %
                                (r["view"], r["seed"], r["log"], r.get("err", "")))
                finds[k] += 1
                os.unlink(r["rec"])
    shutil.rmtree(work, ignore_errors=True)
    for k in sorted(finds):
        print("fuzz FAIL: %s: %d cases" % (k, finds[k]))
    line = ("%d cases (seed %d, %d seed records%s): %d ended 0, %d ended 1, %d known open%s"
            % (a.n, a.seed, len(seeds), ", sanitizer build" if san else "",
               counts.get("replayed", 0), counts.get("refused", 0), sum(kn.values()),
               " (%s)" % ", ".join("%s %d" % (k, kn[k]) for k in sorted(kn)) if kn else ""))
    if bad or finds:
        print("fuzz FAIL: %s; reproducers in %s" % (line, keep))
        sys.exit(1)
    if not a.keep:
        shutil.rmtree(keep, ignore_errors=True)
    print("fuzz ok: %d kept inputs; %s" % (len(rows), line))


if __name__ == "__main__":
    main()
