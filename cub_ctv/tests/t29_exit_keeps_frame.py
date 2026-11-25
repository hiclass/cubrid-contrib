#!/usr/bin/env python3
"""t29: q, Ctrl-C and Ctrl-\\ leave the last frame on the terminal (no server).
Leaving the full screen must not wipe what was shown: after the alternate
screen is left, the frame is drawn once more on the main screen with no
clear and no cursor move, and nothing of the tool stays on the tty: text
attributes off, cursor shown, the terminal modes as they were."""
import os, pty, fcntl, termios, struct, time, select, signal, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.environ.get("CUB_CTV_TOOL", os.path.join(HERE, "..", "cub_ctv"))
REC = os.path.join(HERE, "fixtures", "demodb-live.rec")
SGR = re.compile(rb"\x1b\[([0-9;]*)m")
CSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

def fail(m):
    print("t29 FAIL:", m)
    sys.exit(1)

def run(key, lang):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.pop("COLUMNS", None)
        os.environ.update(LANG="en_US.UTF-8", TERM="xterm")
        os.execv(TOOL, ["cub_ctv", "--replay", REC, "--replay-speed", "0.2", "--" + lang])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    buf, t, sent, status = b"", time.time(), False, None
    while time.time() - t < 20:
        if select.select([fd], [], [], 0.05)[0]:
            try:
                buf += os.read(fd, 1 << 16)
            except OSError:
                pass
        if not sent and b"\x1b[?1049h" in buf and b"LOG" in buf:
            time.sleep(0.3)             # the frame is fully out
            os.write(fd, key)
            sent = True
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

n = 0
for name, key, code in (("q", b"q", 0), ("Ctrl-C", b"\x03", 128 + signal.SIGINT),
                        ("Ctrl-\\", b"\x1c", 128 + signal.SIGQUIT)):
    for lang in ("en", "ko"):
        status, buf, lf = run(key, lang)
        what = "%s (%s)" % (name, lang)
        if not (os.WIFEXITED(status) and os.WEXITSTATUS(status) == code):
            fail("%s: status %#x, wanted exit %d" % (what, status, code))
        if not (lf & termios.ICANON and lf & termios.ECHO):
            fail("%s: the terminal is left raw" % what)
        cut = buf.rfind(b"\x1b[?1049l")
        if cut < 0 or cut < buf.rfind(b"\x1b[?1049h"):
            fail("%s: the alternate screen is not left" % what)
        if buf.rfind(b"\x1b[?25h") < buf.rfind(b"\x1b[?25l"):
            fail("%s: the cursor stays hidden" % what)
        last = buf[buf.rfind(b"\x1b[H\x1b[2J", 0, cut):cut]     # the frame shown last
        kept = buf[cut + len(b"\x1b[?1049l"):]
        # what stays: the same frame, text for text
        text = lambda b: CSI.sub(b"", b).replace(b"\r", b"").strip()
        if not text(kept) or text(kept) != text(last):
            fail("%s: the frame left on the main screen is not the one shown last:\n%r\n%r"
                 % (what, text(last)[:200], text(kept)[:200]))
        # nothing cleared, no cursor move: colors only
        other = [m for m in CSI.findall(kept) if not m.endswith(b"m")]
        if other:
            fail("%s: the kept frame holds %r" % (what, other[:3]))
        # no attribute stays on: the last one is a reset, and no text after it
        sg = list(SGR.finditer(kept))
        if not sg or sg[-1].group(1) not in (b"", b"0"):
            fail("%s: the last text attribute is not a reset" % what)
        if kept[sg[-1].end():].strip(b"\r\n"):
            fail("%s: text after the last reset" % what)
        # the reset of its own closes the frame, whatever the frame ended with
        if not kept.endswith(b"\n\x1b[0m"):
            fail("%s: the kept frame does not end with a reset of its own: %r" % (what, kept[-20:]))
        n += 1
print("t29 PASS: q, Ctrl-C and Ctrl-\\ leave the last frame on the main screen, nothing cleared,"
      " attributes off, cursor and modes back (%d runs)" % n)
