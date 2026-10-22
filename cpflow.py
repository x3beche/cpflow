#!/usr/bin/env python3
"""
cpflow - copy files with a scrolling file list and a fixed progress bar.

Wraps rsync: file names stream by above, one progress bar sits pinned at the
bottom of the terminal and never scrolls away.

MIT License - see LICENSE.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import time

__version__ = "1.0.0"

PROG_RE = re.compile(r'^\s*([\d.,]+)\s*([KMGTP]?i?B?)\s+(\d{1,3})%\s+(\S+)\s+(\d+:\d+:\d+)')
XFR_RE = re.compile(r'\(xfr#(\d+), to-chk=(\d+)/(\d+)\)')
OUT = sys.stdout
SIZE = {"cols": 80, "rows": 24}

HELP = """\
cpflow {v} - copy with streaming file names and a pinned progress bar

usage:
  cpflow [options] SOURCE/ DEST/
  cpflow [options] SOURCE/ DEST/ -- [extra rsync options]

options:
  -n, --dry-run   show what would be copied, copy nothing
  -h, --help      show this help
  -V, --version   show version

examples:
  cpflow /media/user/OLD/ /media/user/NEW/
  cpflow -n /media/user/OLD/ /media/user/NEW/
  cpflow /media/user/OLD/ /media/user/NEW/ -- --exclude=.Trash-1000

A trailing slash on SOURCE means "copy the contents", same as rsync.
Nothing is ever deleted: verify first, remove later.
""".format(v=__version__)


def refresh():
    s = shutil.get_terminal_size((80, 24))
    SIZE["cols"], SIZE["rows"] = s.columns, s.lines
    return SIZE["cols"], SIZE["rows"]


def bottom():
    """Last line of the scrolling region."""
    return max(3, SIZE["rows"] - 2)


def draw_bar(pct, done, rate, eta, left, total):
    cols = SIZE["cols"]
    pct = max(0, min(100, pct))
    if cols < 25:
        OUT.write("\x1b[{};1H\x1b[2K{:3d}%".format(SIZE["rows"] - 1, pct))
        OUT.flush()
        return
    parts = ["{:3d}%".format(pct), "{:>9}".format(done), "{:>10}".format(rate), "{:>8}".format(eta)]
    if left is not None and total:
        parts.append("left:{}/{}".format(left, total))
    while True:                                  # drop detail on narrow terminals
        tail = "  ".join(parts)
        width = cols - len(tail) - 4
        if width >= 10 or len(parts) == 1:
            break
        parts.pop()
    width = max(10, width)
    filled = int(pct * width / 100)
    bar = "#" * filled + "-" * (width - filled)
    OUT.write("\x1b[{};1H\x1b[2K[{}] {}".format(SIZE["rows"] - 1, bar, tail))
    OUT.flush()


def fail(msg, code=2):
    OUT.write("\x1b[r\x1b[?25h" if OUT.isatty() else "")
    sys.exit("cpflow: " + msg)


def parse_args(argv):
    opts, rest, extra = [], [], []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--":
            extra = argv[i + 1:]
            break
        if a in ("-h", "--help"):
            print(HELP, end="")
            sys.exit(0)
        elif a in ("-V", "--version"):
            print("cpflow " + __version__)
            sys.exit(0)
        elif a in ("-n", "--dry-run"):
            opts.append("-n")
        elif a.startswith("-") and len(a) > 1:
            fail("unknown option {!r} (try --help)".format(a))
        else:
            rest.append(a)
        i += 1
    if len(rest) != 2:
        fail("expected SOURCE and DEST (try --help)")
    return opts, rest[0], rest[1], extra


def main():
    opts, src, dst, extra = parse_args(sys.argv[1:])

    if not os.path.exists(src):
        fail("source does not exist: " + src)
    if not os.path.isdir(dst):
        fail("destination is not a directory: " + dst)
    if not src.endswith("/"):
        src += "/"

    cmd = ["rsync", "-rt", "--info=name1,progress2", "--human-readable",
           "--modify-window=1"] + opts + [src, dst] + extra

    # Not a terminal (piped to a file, tee, cron): plain rsync, split lines,
    # no escape codes.
    if not OUT.isatty():
        os.execvp(cmd[0], cmd)

    refresh()
    OUT.write("\x1b[2J\x1b[H\x1b[?25l")
    OUT.write("\x1b[1;{}r".format(bottom()))
    draw_bar(0, "0 B", "", "", None, None)

    def on_winch(signum, frame):
        try:
            refresh()
            OUT.write("\x1b[1;{}r\x1b[{};1H".format(bottom(), bottom()))
            OUT.flush()
        except Exception:
            pass

    try:
        signal.signal(signal.SIGWINCH, on_winch)
    except (AttributeError, ValueError):
        pass

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         bufsize=0, start_new_session=True)

    def scroll(text):
        OUT.write("\x1b[{};1H\n\x1b[2K{}".format(bottom(), text[:SIZE["cols"] - 1]))
        OUT.flush()

    def status(text):
        OUT.write("\x1b[{};1H\x1b[2K{}".format(SIZE["rows"], text[:SIZE["cols"] - 1]))
        OUT.flush()

    buf = b""
    started = time.monotonic()
    rc = None
    try:
        while True:
            chunk = p.stdout.read(65536)
            if not chunk:
                break
            buf += chunk
            while True:
                idx = [i for i in (buf.find(b"\r"), buf.find(b"\n")) if i != -1]
                if not idx:
                    break
                i = min(idx)
                seg = buf[:i].decode("utf-8", "replace")
                buf = buf[i + 1:]
                m = PROG_RE.match(seg)
                if m:                                  # bytes/percent chunk -> bar
                    x = XFR_RE.search(seg)
                    left, total = (int(x.group(2)), int(x.group(3))) if x else (None, None)
                    draw_bar(int(m.group(3)), m.group(1) + m.group(2),
                             m.group(4), m.group(5), left, total)
                elif seg.strip():                      # file name or message
                    scroll(seg.strip())
                    status(seg.strip())
        rc = p.wait()
    except KeyboardInterrupt:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGINT)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(p.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            p.wait()
        rc = 130
    finally:
        OUT.write("\x1b[r\x1b[?25h")
        OUT.write("\x1b[{};1H\x1b[2K".format(SIZE["rows"] - 1))

    elapsed = time.monotonic() - started
    if rc == 0:
        print("done in {:.1f}s".format(elapsed))
    elif rc == 130:
        print("cancelled - run the same command again to resume")
    else:
        print("rsync exited with code {}".format(rc))
    OUT.write("\x1b[2K")
    sys.exit(rc)


if __name__ == "__main__":
    main()
