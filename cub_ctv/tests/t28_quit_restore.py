#!/usr/bin/env python3
"""t28: Ctrl-\\ (SIGQUIT) leaves the terminal as it was (no server).
The raw mode keeps ISIG, so Ctrl-\\ reaches the tool as SIGQUIT: it must put
back the terminal modes and the main screen and end as Ctrl-C does, by an
exit of its own (128 + signal), not killed by the signal: no core file."""
import os, pty, fcntl, termios, struct, time, select, signal, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("CUB_CTV_TOOL", os.path.join(HERE, "..", "cub_ctv"))
REC = os.path.join(HERE, "fixtures", "demodb-live.rec")

def fail(m):
    print("t28 FAIL:", m)
    sys.exit(1)

def run(key, ignore=(), then=None):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.pop("COLUMNS", None)
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
        signal.signal(signal.SIGQUIT, signal.SIG_DFL)
        for g in ignore:
            signal.signal(g, signal.SIG_IGN)
        try:
            import resource
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))   # no core file from the test
        except Exception:
            pass
        os.execv(TOOL, ["cub_ctv", "--replay", REC, "--replay-speed", "0.2"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
    buf, t, sent, status = b"", time.time(), False, None
    while time.time() - t < 20:
        if select.select([fd], [], [], 0.05)[0]:
            try:
                d = os.read(fd, 1 << 16)
            except OSError:
                d = b""
            buf += d
        if not sent and b"\x1b[?1049h" in buf and b"LOG" in buf:
            lf = termios.tcgetattr(fd)[3]
            if lf & (termios.ICANON | termios.ECHO):
                fail("the screen is up but the terminal is not raw")
            os.write(fd, key)
            sent = True
            if then is not None:
                # the key must not end it: still there after a while, then quit
                time.sleep(1.0)
                if os.waitpid(pid, os.WNOHANG)[0]:
                    fail("%r ended the tool though its signal came ignored" % key)
                os.write(fd, then)
        if sent:
            p, st = os.waitpid(pid, os.WNOHANG)
            if p:
                status = st
                break
    if status is None:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)
        fail("%r: the tool did not end" % key)
    time.sleep(0.2)
    try:
        while select.select([fd], [], [], 0)[0]:
            d = os.read(fd, 1 << 16)
            if not d:
                break
            buf += d
    except OSError:
        pass
    lf = termios.tcgetattr(fd)[3]
    os.close(fd)
    return status, buf, lf

for name, key, code in (("Ctrl-\\", b"\x1c", 128 + signal.SIGQUIT), ("Ctrl-C", b"\x03", 128 + signal.SIGINT)):
    status, buf, lf = run(key)
    if not (lf & termios.ICANON and lf & termios.ECHO):
        fail("%s: the terminal is left raw (ICANON %s, ECHO %s)"
             % (name, bool(lf & termios.ICANON), bool(lf & termios.ECHO)))
    if buf.rfind(b"\x1b[?1049l") < buf.rfind(b"\x1b[?1049h"):
        fail("%s: the alternate screen is not left" % name)
    if buf.rfind(b"\x1b[?25h") < buf.rfind(b"\x1b[?25l"):
        fail("%s: the cursor stays hidden" % name)
    if os.WIFSIGNALED(status):
        fail("%s: killed by signal %d (a core file for SIGQUIT)" % (name, os.WTERMSIG(status)))
    if os.WEXITSTATUS(status) != code:
        fail("%s: exit %d, wanted %d" % (name, os.WEXITSTATUS(status), code))
# a signal the caller ignores stays ignored (nohup, a shell with no job control)
for name, key, sig in (("Ctrl-\\", b"\x1c", signal.SIGQUIT), ("Ctrl-C", b"\x03", signal.SIGINT)):
    status, buf, lf = run(key, ignore=(sig,), then=b"q")
    if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
        fail("%s ignored, then q: status %#x" % (name, status))
    if not (lf & termios.ICANON and lf & termios.ECHO):
        fail("%s ignored, then q: the terminal is left raw" % name)
# an ignored SIGTSTP stays ignored after a SIGCONT too
pid, fd = pty.fork()
if pid == 0:
    os.environ.pop("COLUMNS", None)
    os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
    signal.signal(signal.SIGTSTP, signal.SIG_IGN)
    os.execv(TOOL, ["cub_ctv", "--replay", REC, "--replay-speed", "0.2"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
buf, t = b"", time.time()
while time.time() - t < 20 and not (b"\x1b[?1049h" in buf and b"LOG" in buf):
    if select.select([fd], [], [], 0.05)[0]:
        buf += os.read(fd, 1 << 16)
os.kill(pid, signal.SIGCONT)
time.sleep(0.3)
os.kill(pid, signal.SIGTSTP)
time.sleep(0.5)
raw = not (termios.tcgetattr(fd)[3] & termios.ICANON)
os.write(fd, b"q")
t, status = time.time(), None
while time.time() - t < 10 and status is None:
    if select.select([fd], [], [], 0.05)[0]:
        try:
            os.read(fd, 1 << 16)
        except OSError:
            pass
    p, st = os.waitpid(pid, os.WNOHANG)
    status = st if p else None
if status is None:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
    fail("SIGTSTP ignored, SIGCONT then SIGTSTP: q no longer ends the tool")
os.close(fd)
if not raw or not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0):
    fail("SIGTSTP ignored, SIGCONT then SIGTSTP: raw %s, status %#x" % (raw, status))
print("t28 PASS: Ctrl-\\ puts back the terminal modes, the main screen and the cursor and exits 131 (not killed: no core); Ctrl-C 130; a signal that came ignored stays ignored (SIGTSTP after SIGCONT too)")
