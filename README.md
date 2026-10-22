# cpflow

**Copy files with a btop-style terminal UI: file names stream by, the progress bar stays pinned, and resizing the window never breaks the layout.**

Single C file. No rsync, no Python, no runtime dependencies — one binary, run it.

```
╭─ cpflow v1.0.0 ────────────────────────────────────────────────00:01:23─╮
│ /media/user/OLD  →  /media/user/NEW                                      │
│ ██████████████████████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │
│  61%   11.4 GB / 18.6 GB   42.7 MB/s   eta 00:02:41                      │
│                                                                          │
│ ╭─ transfer ───────────────────────────╮ ╭─ speed ────────────────────╮  │
│ │ done    11.4 GB                      │ │            ▁▂▃▅▇█▇▆▅▃▂▁     │  │
│ │ files   324 / 512                    │ │ eta     00:02:41            │  │
│ │ left    188                          │ │ peak    48.2 MB/s           │  │
│ │ now     42.7 MB/s                    │ │ q / Esc / Ctrl-C  cancel    │  │
│ ╰──────────────────────────────────────╯ ╰─────────────────────────────╯  │
│ ╭─ current file ───────────────────────────────────────────────────────╮ │
│ │ /media/user/OLD/media/clip-2026-10-22.mkv                            │ │
│ ╰──────────────────────────────────────────────────────────────────────╯ │
│ ╭─ files ──────────────────────────────────────────────────────────────╮ │
│ │ media/clip-2026-10-21.mkv                                            │ │
│ │ media/clip-2026-10-22.mkv                                            │ │
│ ╰──────────────────────────────────────────────────────────────────────╯ │
╰─ q quit ─────────────────────────────────────────────────────────────────╯
```

<sub>Rendered from an actual 100×30 capture of the program.</sub>

## Why

`rsync --progress` cannot show a scrolling file list *and* a fixed progress bar: it writes the percentage to the same line it later writes file names to, so the two overwrite each other. Doing it properly needs a program that owns the screen.

`cpflow` composes every frame into a cell grid and blits it with absolute cursor positions. There is no scroll region and no partial redraw, so a resize mid-copy just produces a correct frame at the new size.

## Install

Download the binary for your platform from [Releases](../../releases) and put it in your `PATH`:

```bash
# Linux x86_64
curl -L -o cpflow https://github.com/x3beche/cpflow/releases/latest/download/cpflow-linux-x86_64
chmod +x cpflow && sudo mv cpflow /usr/local/bin/
```

The binaries are not code-signed, so macOS quarantines them after download. Clear the flag once:

```bash
xattr -d com.apple.quarantine cpflow-macos-arm64   # or -x86_64
chmod +x cpflow-macos-arm64
```

| Platform | Asset |
|---|---|
| Linux x86_64 | `cpflow-linux-x86_64` |
| Linux aarch64 (ARM64) | `cpflow-linux-aarch64` |
| Linux armv7 (Raspberry Pi 32-bit) | `cpflow-linux-armv7` |
| Windows x86_64 | `cpflow-windows-x86_64.exe` |
| macOS Intel | `cpflow-macos-x86_64` |
| macOS Apple Silicon | `cpflow-macos-arm64` |

All Linux binaries are statically linked: no glibc version roulette, they run on any distro of the same architecture.

### Build from source

```bash
git clone https://github.com/x3beche/cpflow.git
cd cpflow
make            # or: make static
sudo make install
```

Only a C11 compiler is required. `cc -std=c11 -O2 src/cpflow.c -o cpflow` is the whole build.

## Usage

```
cpflow [options] SOURCE DEST

options:
  -n, --dry-run         show what would be copied, copy nothing
  --exclude=PATTERN     skip names matching PATTERN (* and ? work)
  -h, --help            show this help
  -V, --version         show version

keys:
  q, Esc, Ctrl-C        stop copying and exit
```

The **contents** of `SOURCE` are copied into `DEST` (`cp -r SOURCE/. DEST/` semantics, not `cp -r SOURCE DEST/`).

```bash
cpflow /media/user/OLD /media/user/NEW                        # copy
cpflow -n /media/user/OLD /media/user/NEW                     # preview
cpflow --exclude=.Trash-1000 /media/user/OLD /media/user/NEW  # skip junk
```

### Resuming

A file is skipped when the destination already has the same size and modification time (2 second tolerance, because exFAT and FAT store timestamps that coarsely). Kill a copy with `q` and run the same command again — finished files are skipped, the half-written one is copied again from the start.

## Behaviour worth knowing

- **Nothing is ever deleted.** No `--delete`, no cleanup of the destination.
- Symlinks are recreated on POSIX; on Windows they are skipped (creating reparse points needs privileges).
- Modification times are preserved. Permissions are not: files are created `0644` and directories `0777` minus your umask. Windows has no equivalent to copy.
- Read/write errors are reported, the copy continues with the next file, and the exit code is `1` at the end.
- Piped output (`cpflow a b > log`) skips the UI and prints plain lines instead.
- Exit codes: `0` success, `1` finished with file errors, `2` bad arguments, `130` cancelled.

## Safe order: copy, verify, delete

```bash
cpflow /media/user/OLD /media/user/NEW

# verify: silent output means every file is identical
rsync -rtnc --modify-window=2 /media/user/OLD/ /media/user/NEW/

# only then
find /media/user/OLD -mindepth 1 -maxdepth 1 -exec rm -rf {} +
```

Do not use `rm -rf /media/user/OLD/*` — the glob silently skips dotfiles.

## How it works

- `SCAN` walks the tree, counting bytes and files so the bar has a denominator.
- `COPY` walks it again, creating directories and copying files in 1 MiB chunks, updating the UI between chunks and polling the keyboard there too — that is why `q` responds within a few milliseconds even in the middle of a 40 GB file.
- The screen is a `W × H` array of cells (UTF-8 char + RGB). Widgets write into it; `blit()` emits one line per row with absolute positioning and `ESC[K`.

## Platform notes

- Windows 10+ (uses `ENABLE_VIRTUAL_TERMINAL_PROCESSING` for ANSI). Paths go through the wide API, so non-ASCII file names work.
- macOS: builds with clang, no extra flags.
- Resizing is handled on every frame; there is no `SIGWINCH` handler to miss.

## License

MIT — see [LICENSE](LICENSE).
