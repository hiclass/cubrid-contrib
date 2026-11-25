#!/usr/bin/env python3
"""t27: what the verdict says of an engine holder it could not name (no server).
The engine's daemons all run as tran 0.  When the thread seen holding the CS
has no name read, or no single thread is known, the verdict names no daemon
and gives no capacity row (one thread's share is not known)."""
import os, pty, fcntl, termios, struct, time, select, re, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("CUB_CTV_TOOL", os.path.join(HERE, "..", "cub_ctv"))
REC = os.path.join(HERE, "fixtures", "demodb-live.rec")
ESC = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
LOG, THR = "1", "4300"          # the fixture's first frame: log-flush holds LOG

def record(edit):
    """the fixture with its first frame's lines passed through edit()"""
    out, f = [], 0
    for line in open(REC):
        tok = line.split()
        f += tok[:1] == ["F"]
        if f == 1 and tok:
            line = edit(tok, line)
        if line:
            out.append(line)
    t = tempfile.NamedTemporaryFile("w", suffix=".rec", delete=False)
    t.write("".join(out))
    t.close()
    return t.name

def verdict(rec, lang):
    """item -> text of the first frame's verdict rows"""
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.pop("COLUMNS", None)
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
        os.execv(TOOL, ["cub_ctv", "--replay", rec, "--replay-speed", "4", "--" + lang])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
    buf, t = b"", time.time()
    while time.time() - t < 2.5:
        if select.select([fd], [], [], 0.05)[0]:
            try:
                d = os.read(fd, 1 << 16)
            except OSError:
                break
            if not d:
                break
            buf += d
    try:
        os.kill(pid, 15)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    frames = buf.decode("utf-8", "replace").split("\x1b[H\x1b[2J")
    rows = {}
    for l in ESC.sub("", frames[1] if len(frames) > 1 else "").replace("\r", "").split("\n"):
        m = re.match(r"  (\S+) +(.*)", l)
        if m:
            rows.setdefault(m.group(1), m.group(2))
    return rows

def fail(m):
    print("t27 FAIL:", m)
    sys.exit(1)

WHO = {"en": "who", "ko": "누가"}
CAP = {"en": "capacity", "ko": "처리량"}
NAMED = {"en": "engine thread log-flush", "ko": "엔진 스레드 log-flush"}
UNNAMED = {"en": "engine daemon (system tran)", "ko": "엔진 데몬 (system tran)"}
GUESS = ("log flush daemon", "로그 플러시 데몬")

as_is = record(lambda tok, line: line)
# the holder's thread states were never read: its name is not known
no_name = record(lambda tok, line: "" if tok[0] == "O" and tok[1] == THR else line)
# the holding thread itself is not known (tran 0 only)
def drop_thr(tok, line):
    if tok[0] == "W" and tok[1] == LOG and tok[3] == "0":
        tok[4] = "-1"
        return " ".join(tok) + "\n"
    return line
no_thr = record(drop_thr)
try:
    for lang in ("en", "ko"):
        v = verdict(as_is, lang)
        if not v.get(WHO[lang], "").startswith(NAMED[lang]) or CAP[lang] not in v:
            fail("%s: a named daemon: %r" % (lang, v))
        v = verdict(no_name, lang)
        if not v.get(WHO[lang], "").startswith(UNNAMED[lang]) or any(g in str(v) for g in GUESS):
            fail("%s: no name read, yet a daemon is named: %r" % (lang, v.get(WHO[lang])))
        if CAP[lang] not in v:
            fail("%s: one thread known, no capacity row" % lang)
        v = verdict(no_thr, lang)
        if not v.get(WHO[lang], "").startswith(UNNAMED[lang]) or any(g in str(v) for g in GUESS):
            fail("%s: no thread known, yet a daemon is named: %r" % (lang, v.get(WHO[lang])))
        if CAP[lang] in v:
            fail("%s: no single thread known, yet a capacity row: %r" % (lang, v[CAP[lang]]))
finally:
    for p in (as_is, no_name, no_thr):
        os.unlink(p)
print("t27 PASS: an engine holder with no name read is not called the log flush daemon;"
      " no capacity row without a single thread (en/ko)")
