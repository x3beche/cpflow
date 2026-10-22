# cpflow

**Copy files with a scrolling file list and a progress bar that stays pinned at the bottom of your terminal.**

`cpflow` is a thin wrapper around `rsync`. You get the reliability of `rsync` (resumable, verifiable, no surprises) with an output that you can actually watch: every file name streams by, while one single progress bar sits at the bottom and never scrolls away.

```
d-24.bin
d-25.bin
alt klasor/
alt klasor/p-1.dat
alt klasor/p-2.dat
alt klasor/p-3.dat
[#######################-----------------]  61%   11.4G  42.7MB/s   0:02:41  left:188/512
alt klasor/p-3.dat
```

<sub>That is the real layout, not a mock-up: the last two lines are reserved, everything above them scrolls.</sub>

## Why

`rsync --progress` cannot do this. It writes the percentage to the same line it later writes file names to (it returns to column 0 with `\r`), so names and numbers overwrite each other:

```
f1.bin
        32.77K   3%    0.00kB/s    0:00:00
f2.bin                    <- lands on top of the line above
```

`--info=progress2` is not better: it gives you one global bar, but per-file names still collide with it. Terminals have no way to say "these two lines are mine, never scroll them" unless a program asks for it — so `cpflow` does: it sets a scroll region for the top of the screen and draws the bar below it.

## Requirements

- Linux or macOS (uses ANSI escape codes and `SIGWINCH`)
- `rsync` 3.x in `PATH`
- Python 3.7+ (standard library only, nothing to install)

## Install

```bash
git clone https://github.com/x3beche/cpflow.git
install -m 755 cpflow/cpflow.py ~/.local/bin/cpflow
```

Or by hand — it is a single file with no dependencies:

```bash
cp cpflow.py ~/.local/bin/cpflow
chmod +x ~/.local/bin/cpflow
```

Make sure `~/.local/bin` is in your `PATH`.

## Usage

```bash
cpflow SOURCE/ DEST/
```

A trailing slash on `SOURCE` means "copy the contents", exactly like `rsync`. Leave it off and you get a `SOURCE` directory inside `DEST`.

```
cpflow 1.0.0 - copy with streaming file names and a pinned progress bar

usage:
  cpflow [options] SOURCE/ DEST/
  cpflow [options] SOURCE/ DEST/ -- [extra rsync options]

options:
  -n, --dry-run   show what would be copied, copy nothing
  -h, --help      show this help
  -V, --version   show version
```

### Examples

Copy one USB disk onto another:

```bash
cpflow /media/user/OLD/ /media/user/NEW/
```

See what would happen first:

```bash
cpflow -n /media/user/OLD/ /media/user/NEW/
```

Skip the trash folder and let `rsync` verbose-log:

```bash
cpflow /media/user/OLD/ /media/user/NEW/ -- --exclude=.Trash-1000
```

Resume an interrupted copy — just run the same command again:

```bash
cpflow /media/user/OLD/ /media/user/NEW/
```

## What it does with your terminal

- The top `rows - 2` lines form a scroll region; file names appear there and push older ones off the screen.
- The bottom two lines are reserved: the bar (percentage, bytes done, speed, elapsed, files left) and the name of the file currently being transferred.
- Narrow terminals lose detail instead of wrapping: speed and ETA are dropped first, and under 25 columns only the percentage is shown.
- Resizing the window re-lays out the scroll region (`SIGWINCH`).
- `Ctrl-C` forwards `SIGINT` to the whole `rsync` process group, waits, escalates to `SIGKILL` if needed, and restores the terminal. No orphan `rsync` left behind.
- If stdout is **not** a terminal (a pipe, a log file, cron), `cpflow` skips the UI entirely and `exec`s plain `rsync`, so `cpflow a/ b/ | tee copy.log` behaves sanely.

## Safety

`cpflow` copies. It never deletes anything, and it never passes `--delete` on your behalf.

The safe order is always: **copy, verify, delete.**

```bash
cpflow /media/user/OLD/ /media/user/NEW/

# empty output means every file is byte-identical
rsync -rtn --checksum --modify-window=1 /media/user/OLD/ /media/user/NEW/

# only after that is clean
find /media/user/OLD -mindepth 1 -maxdepth 1 -exec rm -rf {} +
```

Do not use `rm -rf /media/user/OLD/*` for that last step: the glob silently skips dotfiles such as `.config` or `.Trash-1000`.

## How it works

1. Runs `rsync -rt --info=name1,progress2 --human-readable --modify-window=1`.
2. Reads its output byte by byte and splits on `\r` and `\n`. Segments terminated by `\r` are progress updates (`  61%  11.4G  42.7MB/s  0:02:41 (xfr#324, to-chk=188/512)`); segments terminated by `\n` are file names or messages.
3. File names go to the scroll region, percentages go to the pinned bar.

`--modify-window=1` is there because exFAT and FAT store timestamps with 2-second resolution; without it `rsync` thinks freshly copied files changed and re-copies them on every run.

Flags are deliberately conservative: `-rt` and no `-a`, since exFAT cannot store Unix permissions or ownership and `-a` only produces noise (`chmod ... failed`). If your destination is ext4 and you want them preserved, pass them through: `cpflow src/ dst/ -- -a`.

## Notes

- `cpflow` shows you what `rsync` reports; it does not add verification of its own. Use the checksum command above when it matters.
- The percentage is `rsync`'s own, counted over the bytes it still has to send — so an interrupted-and-resumed copy starts at 0% again for the remaining work.
- Tested on Linux with `rsync` 3.2.7 and Python 3.10–3.12.

## License

MIT — see [LICENSE](LICENSE).
