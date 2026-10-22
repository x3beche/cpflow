/*
 * cpflow - copy files with a btop-style terminal UI.
 *
 * Self-contained: no rsync, no Python, no runtime dependencies. The copy
 * engine is in this file; the screen is composed into a cell grid and
 * blitted line by line every frame, so resizing the terminal can never
 * corrupt the layout.
 *
 * MIT License - see LICENSE.
 */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#define VERSION "1.0.0"
#define PATHMAX 1024
#define CHUNK (1024u * 1024u)
#define MAX_EXCLUDES 32
#define HIST 60
#define NAMES 64
#define MTIME_TOL 2000 /* ms: exFAT/FAT store 2 s resolution */

/* ------------------------------------------------------------------ util -- */

static double now_sec(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static size_t u8len(const char *s)
{
    size_t n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Copy at most `cols` code points, adding an ellipsis when cut. */
static void u8fit(char *out, size_t cap, const char *s, size_t cols)
{
    size_t i = 0, n = 0;
    if (cap == 0) return;
    if (u8len(s) <= cols) {
        snprintf(out, cap, "%s", s);
        return;
    }
    while (s[i] && n < cols - 1) {
        size_t step = 1;
        unsigned char c = (unsigned char)s[i];
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        if (i + step + 3 >= cap) break;
        i += step;
        n++;
    }
    memcpy(out, s, i);
    memcpy(out + i, "\xE2\x80\xA6", 3);
    out[i + 3] = 0;
}

static void human(uint64_t bytes, char *out, size_t cap)
{
    double v = (double)bytes;
    if (v >= 1e9) snprintf(out, cap, "%.2f GB", v / 1e9);
    else if (v >= 1e6) snprintf(out, cap, "%.1f MB", v / 1e6);
    else if (v >= 1e3) snprintf(out, cap, "%.1f KB", v / 1e3);
    else snprintf(out, cap, "%llu B", (unsigned long long)bytes);
}

static void human_rate(uint64_t bps, char *out, size_t cap)
{
    double v = (double)bps;
    if (v >= 1e9) snprintf(out, cap, "%.1f GB/s", v / 1e9);
    else if (v >= 1e6) snprintf(out, cap, "%.1f MB/s", v / 1e6);
    else if (v >= 1e3) snprintf(out, cap, "%.1f KB/s", v / 1e3);
    else snprintf(out, cap, "%.0f B/s", v);
}

static void clock_str(double seconds, char *out, size_t cap)
{
    long s = (seconds < 0) ? -1 : (long)seconds;
    if (s < 0) { snprintf(out, cap, "--:--:--"); return; }
    snprintf(out, cap, "%02ld:%02ld:%02ld", s / 3600, (s % 3600) / 60, s % 60);
}

/* '*' and '?' wildcards. */
static int wildmatch(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (wildmatch(p, s)) return 1;
            return 0;
        }
        if (*p == '?') {
            if (!*s) return 0;
            p++; s++;
            continue;
        }
#if defined(_WIN32)
        if (tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
#else
        if ((unsigned char)*p != (unsigned char)*s) return 0;
#endif
        p++; s++;
        if (!*s && *p) return 0;
    }
    return *s == 0;
}

/* --------------------------------------------------------------- platform -- */

#if defined(_WIN32)

static HANDLE g_hout, g_hin;
static DWORD g_outmode, g_inmode;

static void term_init(void)
{
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(g_hout, &g_outmode);
    GetConsoleMode(g_hin, &g_inmode);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(g_hout, g_outmode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleMode(g_hin, (g_inmode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                        ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE)) |
                              ENABLE_EXTENDED_FLAGS);
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);
}

static void term_done(void)
{
    fputs("\x1b[?25h\x1b[?1049l", stdout);
    fflush(stdout);
    SetConsoleMode(g_hout, g_outmode);
    SetConsoleMode(g_hin, g_inmode);
}

static void term_size(int *w, int *h)
{
    CONSOLE_SCREEN_BUFFER_INFO si;
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(g_hout, &si)) {
        *w = si.srWindow.Right - si.srWindow.Left + 1;
        *h = si.srWindow.Bottom - si.srWindow.Top + 1;
    } else {
        *w = 80;
        *h = 24;
    }
}

static int key_read(void)
{
    DWORD n = 0;
    if (!GetNumberOfConsoleInputEvents(g_hin, &n) || n == 0) return 0;
    while (n--) {
        INPUT_RECORD rec;
        DWORD got = 0;
        if (!ReadConsoleInputW(g_hin, &rec, 1, &got) || got == 0) break;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;
        WCHAR ch = rec.Event.KeyEvent.uChar.UnicodeChar;
        if (rec.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) return 27;
        if (ch == L'q' || ch == L'Q') return 'q';
        if (ch == 3) return 3;
    }
    return 0;
}

static wchar_t *widen(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) w[0] = 0;
    return w;
}

static char *narrow(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)(n > 0 ? n : 1));
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) s[0] = 0;
    return s;
}

typedef struct { HANDLE h; WIN32_FIND_DATAW fd; int done; } DirIter;

static int dir_open(const char *path, DirIter *it)
{
    wchar_t *wp = widen(path);
    wchar_t pat[PATHMAX + 8];
    if (!wp) return -1;
    _snwprintf(pat, PATHMAX + 7, L"%s\\*", wp);
    free(wp);
    it->h = FindFirstFileW(pat, &it->fd);
    it->done = (it->h == INVALID_HANDLE_VALUE);
    return it->done ? -1 : 0;
}

static int dir_next(DirIter *it, char **name, int *isdir, uint64_t *size, int *islink)
{
    while (!it->done) {
        if (wcscmp(it->fd.cFileName, L".") && wcscmp(it->fd.cFileName, L"..")) {
            *name = narrow(it->fd.cFileName);
            *isdir = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            *islink = (it->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            *size = ((uint64_t)it->fd.nFileSizeHigh << 32) | it->fd.nFileSizeLow;
            if (!*name) return 0;
            if (!FindNextFileW(it->h, &it->fd)) it->done = 1;
            return 1;
        }
        if (!FindNextFileW(it->h, &it->fd)) it->done = 1;
    }
    return 0;
}

static void dir_close(DirIter *it)
{
    if (!it->done) FindClose(it->h);
    it->done = 1;
}

static int fs_mkdir(const char *path)
{
    wchar_t *w = widen(path);
    int ok = w ? CreateDirectoryW(w, NULL) : 0;
    free(w);
    return ok ? 0 : -1;
}

static int fs_exists(const char *path)
{
    wchar_t *w = widen(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

static int fs_isdir(const char *path)
{
    wchar_t *w = widen(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int fs_meta(const char *path, uint64_t *size, int64_t *mtime_ms)
{
    wchar_t *w = widen(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) {
        free(w);
        return -1;
    }
    free(w);
    *size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    {
        /* FILETIME: 100 ns ticks since 1601; convert to Unix ms */
        uint64_t ticks = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        *mtime_ms = (int64_t)(ticks / 10000ULL) - 11644473600000LL;
    }
    return 0;
}

typedef struct { HANDLE h; } File;

static int fs_open_read(const char *path, File *f)
{
    wchar_t *w = widen(path);
    f->h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
             : INVALID_HANDLE_VALUE;
    free(w);
    return f->h == INVALID_HANDLE_VALUE ? -1 : 0;
}

static int fs_open_write(const char *path, File *f)
{
    wchar_t *w = widen(path);
    f->h = w ? CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
             : INVALID_HANDLE_VALUE;
    free(w);
    return f->h == INVALID_HANDLE_VALUE ? -1 : 0;
}

static long fs_read(File *f, void *buf, size_t n)
{
    DWORD got = 0;
    if (!ReadFile(f->h, buf, (DWORD)n, &got, NULL)) return -1;
    return (long)got;
}

static long fs_write(File *f, const void *buf, size_t n)
{
    DWORD put = 0;
    if (!WriteFile(f->h, buf, (DWORD)n, &put, NULL)) return -1;
    return (long)put;
}

static void fs_close(File *f)
{
    if (f->h != INVALID_HANDLE_VALUE) CloseHandle(f->h);
    f->h = INVALID_HANDLE_VALUE;
}

static void fs_set_mtime(const char *path, int64_t mtime_ms)
{
    wchar_t *w = widen(path);
    HANDLE h;
    if (!w) return;
    h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        uint64_t ticks = (uint64_t)(mtime_ms + 11644473600000LL) * 10000ULL;
        FILETIME ft;
        ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFF);
        ft.dwHighDateTime = (DWORD)(ticks >> 32);
        SetFileTime(h, NULL, NULL, &ft);
        CloseHandle(h);
    }
    free(w);
}

static int fs_symlink(const char *target, const char *linkpath)
{
    (void)target;
    (void)linkpath;
    return -1; /* reparse points need privileges; out of scope for v1 */
}

static int fs_readlink(const char *path, char *out, size_t cap)
{
    (void)path;
    (void)out;
    (void)cap;
    return -1;
}

#else /* POSIX */

static struct termios g_saved;
static int g_raw, g_term_up;

static void term_restore(void)
{
    if (g_term_up) {
        fputs("\x1b[?25h\x1b[?1049l", stdout);
        fflush(stdout);
        g_term_up = 0;
    }
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
        g_raw = 0;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    term_restore();
    _exit(130);
}

static void term_init(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &g_saved) == 0) {
        t = g_saved;
        t.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_iflag &= (tcflag_t)~(IXON | ICRNL);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
        g_raw = 1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGQUIT, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGABRT, on_signal);
    fputs("\x1b[?1049h\x1b[?25l", stdout);
    fflush(stdout);
    g_term_up = 1;
}

static void term_done(void) { term_restore(); }

static void term_size(int *w, int *h)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80;
        *h = 24;
    }
}

static int key_read(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == 27) return 27;
    if (c == 'q' || c == 'Q') return 'q';
    if (c == 3) return 3;
    return 1;
}

typedef struct { DIR *d; char path[PATHMAX]; } DirIter;

static int dir_open(const char *path, DirIter *it)
{
    snprintf(it->path, sizeof it->path, "%s", path);
    it->d = opendir(path);
    return it->d ? 0 : -1;
}

static int dir_next(DirIter *it, char **name, int *isdir, uint64_t *size, int *islink)
{
    struct dirent *e;
    while ((e = readdir(it->d)) != NULL) {
        char full[PATHMAX * 2];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(full, sizeof full, "%s/%s", it->path, e->d_name);
        if (lstat(full, &st) != 0) continue;
        *name = strdup(e->d_name);
        if (!*name) return 0;
        *isdir = S_ISDIR(st.st_mode);
        *islink = S_ISLNK(st.st_mode);
        *size = (uint64_t)st.st_size;
        return 1;
    }
    return 0;
}

static void dir_close(DirIter *it)
{
    if (it->d) closedir(it->d);
    it->d = NULL;
}

static int fs_mkdir(const char *path) { return mkdir(path, 0777) == 0 ? 0 : -1; }

static int fs_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int fs_isdir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int fs_meta(const char *path, uint64_t *size, int64_t *mtime_ms)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    *size = (uint64_t)st.st_size;
    *mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000LL + st.st_mtim.tv_nsec / 1000000LL;
    return 0;
}

typedef struct { int fd; } File;

static int fs_open_read(const char *path, File *f)
{
    f->fd = open(path, O_RDONLY);
    return f->fd < 0 ? -1 : 0;
}

static int fs_open_write(const char *path, File *f)
{
    f->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return f->fd < 0 ? -1 : 0;
}

static long fs_read(File *f, void *buf, size_t n) { return (long)read(f->fd, buf, n); }

static long fs_write(File *f, const void *buf, size_t n)
{
    long total = 0;
    const char *p = (const char *)buf;
    while ((size_t)total < n) {
        long w = (long)write(f->fd, p + total, n - (size_t)total);
        if (w <= 0) return -1;
        total += w;
    }
    return total;
}

static void fs_close(File *f)
{
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
}

static void fs_set_mtime(const char *path, int64_t mtime_ms)
{
    struct timespec ts[2];
    ts[0].tv_sec = (time_t)(mtime_ms / 1000);
    ts[0].tv_nsec = (long)(mtime_ms % 1000) * 1000000L;
    ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW);
}

static int fs_symlink(const char *target, const char *linkpath) { return symlink(target, linkpath); }

static int fs_readlink(const char *path, char *out, size_t cap)
{
    ssize_t n = readlink(path, out, cap - 1);
    if (n < 0) return -1;
    out[n] = 0;
    return 0;
}

#endif

/* --------------------------------------------------------------- engine --- */

typedef struct {
    char src[PATHMAX], dst[PATHMAX];
    uint64_t total_bytes, total_files;
    uint64_t done_bytes, done_files;
    uint64_t cur_size, cur_done;
    char cur[PATHMAX * 2];
    char names[NAMES][200];
    int name_head, name_count;
    uint64_t hist[HIST];
    int hist_n;
    uint64_t peak;
    double t0, last_draw, last_sample;
    uint64_t sample_acc;
    int sample_n;
    int scanning, cancelled, failed, dry, tty;
    unsigned long scanned;
} App;

static App g;

static void push_name(const char *name)
{
    if (g.name_count > 0) {
        int last = (g.name_head + NAMES - 1) % NAMES;
        if (!strcmp(g.names[last], name)) return;
    }
    {
        size_t n = strlen(name);
        if (n > sizeof g.names[0] - 1) n = sizeof g.names[0] - 1;
        memcpy(g.names[g.name_head], name, n);
        g.names[g.name_head][n] = 0;
    }
    g.name_head = (g.name_head + 1) % NAMES;
    if (g.name_count < NAMES) g.name_count++;
}

static double elapsed(void) { return now_sec() - g.t0; }

static int pct_now(void)
{
    if (!g.total_bytes) return 0;
    uint64_t d = g.done_bytes > g.total_bytes ? g.total_bytes : g.done_bytes;
    return (int)(d * 100 / g.total_bytes);
}

static double eta_seconds(void)
{
    double e = elapsed();
    if (e < 0.5 || g.done_bytes == 0 || g.done_bytes >= g.total_bytes) return -1;
    double rate = (double)g.done_bytes / e;
    return rate > 0 ? (double)(g.total_bytes - g.done_bytes) / rate : -1;
}

static void maybe_sample(void)
{
    double t = now_sec();
    if (t - g.last_sample < 0.5) return;
    {
        uint64_t v = g.sample_n ? g.sample_acc / (uint64_t)g.sample_n : 0;
        if (g.hist_n < HIST) {
            g.hist[g.hist_n++] = v;
        } else {
            memmove(g.hist, g.hist + 1, sizeof(uint64_t) * (HIST - 1));
            g.hist[HIST - 1] = v;
        }
        if (v > g.peak) g.peak = v;
    }
    g.sample_acc = 0;
    g.sample_n = 0;
    g.last_sample = t;
}

/* -------------------------------------------------------------- screen ---- */

typedef struct { char ch[5]; uint8_t r, g_, b, set; } Cell;
static Cell *grid;
static int gw, gh;

enum { COL_DEFAULT, COL_BORDER, COL_TITLE, COL_LABEL, COL_VALUE, COL_ACC, COL_WARN, COL_ERR, COL_DIM };
static const uint8_t palette[][3] = {
    { 0, 0, 0 }, { 48, 66, 74 }, { 0, 215, 190 }, { 130, 150, 160 },
    { 225, 235, 240 }, { 120, 225, 150 }, { 240, 200, 90 }, { 240, 110, 110 },
    { 90, 105, 112 }
};

static void grid_alloc(int w, int h)
{
    if (w == gw && h == gh && grid) return;
    free(grid);
    gw = w; gh = h;
    grid = (Cell *)calloc((size_t)w * (size_t)h, sizeof(Cell));
    if (!grid) { gw = gh = 0; return; }
    for (int i = 0; i < w * h; i++) { grid[i].ch[0] = ' '; grid[i].ch[1] = 0; }
}

static void put(int x, int y, const char *s, int color)
{
    if (y < 0 || y >= gh) return;
    while (*s && x < gw) {
        size_t step = 1;
        unsigned char c = (unsigned char)*s;
        if (c >= 0xF0) step = 4;
        else if (c >= 0xE0) step = 3;
        else if (c >= 0xC0) step = 2;
        if (x >= 0) {
            Cell *cell = &grid[y * gw + x];
            memcpy(cell->ch, s, step);
            cell->ch[step] = 0;
            cell->set = 1;
            cell->r = palette[color][0];
            cell->g_ = palette[color][1];
            cell->b = palette[color][2];
        }
        s += step;
        x++;
    }
}

static void put_clip(int x, int y, const char *s, int color, int maxx)
{
    char buf[PATHMAX * 2];
    size_t room = maxx > x ? (size_t)(maxx - x) : 0;
    if (y < 0 || y >= gh || room == 0) return;
    u8fit(buf, sizeof buf, s, room);
    put(x, y, buf, color);
}

static void fill(int x, int y, int w, int h, int color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, " ", color);
}

static void box(int x, int y, int w, int h, const char *title, int color)
{
    if (w < 3 || h < 2) return;
    put(x, y, "╭", COL_BORDER);
    for (int i = 1; i < w - 1; i++) put(x + i, y, "─", COL_BORDER);
    put(x + w - 1, y, "╮", COL_BORDER);
    for (int j = 1; j < h - 1; j++) {
        put(x, y + j, "│", COL_BORDER);
        for (int i = 1; i < w - 1; i++) put(x + i, y + j, " ", COL_DEFAULT);
        put(x + w - 1, y + j, "│", COL_BORDER);
    }
    put(x, y + h - 1, "╰", COL_BORDER);
    for (int i = 1; i < w - 1; i++) put(x + i, y + h - 1, "─", COL_BORDER);
    put(x + w - 1, y + h - 1, "╯", COL_BORDER);
    if (title && *title && w > 8) {
        char t[128];
        u8fit(t, sizeof t, title, (size_t)(w - 7));
        put(x + 2, y, " ", COL_BORDER);
        put(x + 3, y, t, color);
        put(x + 3 + (int)u8len(t), y, " ", COL_BORDER);
    }
}

static void blit(void)
{
    for (int y = 0; y < gh; y++) {
        printf("\x1b[%d;1H", y + 1);
        int last = -1, lastr = 0, lastg = 0, lastb = 0;
        for (int x = 0; x < gw; x++) {
            Cell *c = &grid[y * gw + x];
            if (c->set) {
                if (!last || c->r != lastr || c->g_ != lastg || c->b != lastb) {
                    printf("\x1b[38;2;%d;%d;%dm", c->r, c->g_, c->b);
                    lastr = c->r; lastg = c->g_; lastb = c->b;
                }
                last = 1;
                fputs(c->ch, stdout);
            } else {
                if (last != 0) { fputs("\x1b[39m", stdout); last = 0; }
                fputs(c->ch[0] ? c->ch : " ", stdout);
            }
        }
        fputs("\x1b[0m\x1b[K", stdout);
    }
    fflush(stdout);
}

static void grad(int i, int width, int *r, int *gr, int *b)
{
    double t = width > 1 ? (double)i / (double)width : 0;
    if (t < 0.55) {
        double k = t / 0.55;
        *r = (int)(0 + 60 * k);
        *gr = (int)(200 + 35 * k);
        *b = (int)(255 - 85 * k);
    } else {
        double k = (t - 0.55) / 0.45;
        *r = (int)(60 + 175 * k);
        *gr = (int)(235 - 30 * k);
        *b = (int)(170 - 90 * k);
    }
}

static void bar(int x, int y, int w, int pct)
{
    int filled = (int)((long long)w * pct / 100);
    for (int i = 0; i < w; i++) {
        if (i < filled) {
            int r, gg, b;
            grad(i, w, &r, &gg, &b);
            Cell *c = &grid[y * gw + (x + i)];
            memcpy(c->ch, "█", 4);
            c->ch[4] = 0;
            c->set = 1;
            c->r = (uint8_t)r; c->g_ = (uint8_t)gg; c->b = (uint8_t)b;
        } else {
            put(x + i, y, "░", COL_DIM);
        }
    }
}

static void spark(int x, int y, int w)
{
    static const char *levels[8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    int n = g.hist_n < w ? g.hist_n : w;
    uint64_t mx = 1;
    for (int i = 0; i < n; i++) if (g.hist[g.hist_n - n + i] > mx) mx = g.hist[g.hist_n - n + i];
    for (int i = 0; i < n; i++) {
        uint64_t v = g.hist[g.hist_n - n + i];
        int lvl = (int)((double)v / (double)mx * 7.0);
        if (lvl > 7) lvl = 7;
        put(x + (w - n) + i, y, v ? levels[lvl] : "▁", v ? COL_ACC : COL_DIM);
    }
}

static void draw(void)
{
    int W, H, pct;
    char buf[PATHMAX * 2 + 32], tmp[64];
    double el = elapsed();

    term_size(&W, &H);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    grid_alloc(W, H);
    if (!grid) return;
    g.last_draw = now_sec();

    fill(0, 0, W, H, COL_DEFAULT);

    if (W < 40 || H < 8) {
        snprintf(tmp, sizeof tmp, "%d%%", pct_now());
        put(0, 0, tmp, COL_ACC);
        if (W > 10) put_clip(6, 0, g.cur, COL_VALUE, W);
        blit();
        return;
    }

    pct = pct_now();
    snprintf(buf, sizeof buf, "cpflow v" VERSION);
    box(0, 0, W, H, buf, COL_TITLE);
    {
        char right[32];
        clock_str(el, right, sizeof right);
        put(W - (int)strlen(right) - 2, 0, right, COL_LABEL);
        put(2, H - 1, " q ", COL_TITLE);
        put(5, H - 1, "quit ", COL_DIM);
    }

    /* paths */
    snprintf(buf, sizeof buf, "%s  →  %s", g.src, g.dst);
    put_clip(2, 1, buf, COL_VALUE, W - 2);

    /* meter */
    bar(2, 2, W - 4, pct);

    /* numbers */
    if (g.scanning) {
        snprintf(buf, sizeof buf, "scanning…  %lu entries", g.scanned);
        put_clip(2, 3, buf, COL_WARN, W - 2);
    } else {
        char done[32], tot[32], rate[32], eta[32];
        human(g.done_bytes, done, sizeof done);
        human(g.total_bytes, tot, sizeof tot);
        human_rate(g.hist_n ? g.hist[g.hist_n - 1] : 0, rate, sizeof rate);
        clock_str(eta_seconds(), eta, sizeof eta);
        snprintf(buf, sizeof buf, "%3d%%   %s / %s   %s   eta %s", pct, done, tot, rate, eta);
        put_clip(2, 3, buf, COL_TITLE, W - 2);
    }

    if (H >= 20 && W >= 64 && !g.scanning) {
        int lw = (W - 2) / 2;
        int rw = (W - 2) - lw;
        int y = 5, ph = 8;

        box(1, y, lw, ph, "transfer", COL_TITLE);
        {
            char v[64];
            human(g.done_bytes, v, sizeof v);
            snprintf(buf, sizeof buf, "done    %s", v);
            put_clip(3, y + 1, buf, COL_VALUE, 1 + lw - 2);
            snprintf(buf, sizeof buf, "files   %llu / %llu",
                     (unsigned long long)g.done_files, (unsigned long long)g.total_files);
            put_clip(3, y + 2, buf, COL_VALUE, 1 + lw - 2);
            snprintf(buf, sizeof buf, "left    %llu",
                     (unsigned long long)(g.total_files > g.done_files ? g.total_files - g.done_files : 0));
            put_clip(3, y + 3, buf, COL_VALUE, 1 + lw - 2);
            human_rate(g.hist_n ? g.hist[g.hist_n - 1] : 0, v, sizeof v);
            snprintf(buf, sizeof buf, "now     %s", v);
            put_clip(3, y + 4, buf, COL_VALUE, 1 + lw - 2);
            human_rate(g.peak, v, sizeof v);
            snprintf(buf, sizeof buf, "peak    %s", v);
            put_clip(3, y + 5, buf, COL_LABEL, 1 + lw - 2);
            clock_str(el, tmp, sizeof tmp);
            snprintf(buf, sizeof buf, "time    %s", tmp);
            put_clip(3, y + 6, buf, COL_LABEL, 1 + lw - 2);
        }

        box(1 + lw, y, rw, ph, "speed", COL_TITLE);
        spark(3 + lw, y + 1, rw - 4);
        {
            char eta[32];
            clock_str(eta_seconds(), eta, sizeof eta);
            snprintf(buf, sizeof buf, "eta     %s", eta);
            put_clip(3 + lw, y + 3, buf, COL_LABEL, 1 + lw + rw - 2);
            human_rate(g.peak, tmp, sizeof tmp);
            snprintf(buf, sizeof buf, "peak    %s", tmp);
            put_clip(3 + lw, y + 4, buf, COL_LABEL, 1 + lw + rw - 2);
            put(3 + lw, y + 5, "q / Esc / Ctrl-C  cancel", COL_DIM);
        }

        box(1, y + ph, W - 2, 3, "current file", COL_WARN);
        put_clip(3, y + ph + 1, g.cur[0] ? g.cur : "waiting…", COL_VALUE, W - 3);

        {
            int ly = y + ph + 3;
            int lh = H - 1 - ly;
            if (lh >= 3) {
                box(1, ly, W - 2, lh, "files", COL_TITLE);
                for (int i = 0; i < lh - 2; i++) {
                    int idx = g.name_count - (lh - 2) + i;
                    if (idx >= 0 && idx < g.name_count) {
                        int real = (g.name_head - g.name_count + idx + NAMES * 2) % NAMES;
                        put_clip(3, ly + 1 + i, g.names[real],
                                 idx == g.name_count - 1 ? COL_ACC : COL_LABEL, W - 3);
                    }
                }
            }
        }
    } else {
        box(1, 5, W - 2, 3, "current file", COL_WARN);
        put_clip(3, 6, g.cur[0] ? g.cur : "waiting…", COL_VALUE, W - 3);
    }

    blit();
}

static void maybe_draw(void)
{
    if (!g.tty) return;
    if (now_sec() - g.last_draw >= 0.08) draw();
}

/* --------------------------------------------------------------- copy ----- */

static int excluded(const char *name, char **ex, int nex)
{
    for (int i = 0; i < nex; i++)
        if (wildmatch(ex[i], name)) return 1;
    return 0;
}

static int cancelled_poll(void)
{
    int k;
    while ((k = key_read()) != 0) {
        if (k == 27 || k == 'q' || k == 3) {
            g.cancelled = 1;
            return 1;
        }
    }
    return 0;
}

static void scan_tree(const char *path)
{
    DirIter it;
    char *name = NULL;
    int isdir, islink;
    uint64_t size;

    if (dir_open(path, &it) != 0) return;
    while (dir_next(&it, &name, &isdir, &size, &islink)) {
        if (cancelled_poll()) { free(name); break; }
        if (isdir && !islink) {
            char sub[PATHMAX * 2];
            snprintf(sub, sizeof sub, "%s/%s", path, name);
            scan_tree(sub);
        } else if (!islink) {
            g.total_bytes += size;
            g.total_files++;
        }
        g.scanned++;
        free(name);
        if ((g.scanned & 0x3F) == 0) {
            maybe_draw();
            maybe_sample();
        }
    }
    dir_close(&it);
}

/* returns 0 on success */
static int copy_file(const char *src, const char *dst, uint64_t size)
{
    File in = { 0 }, out = { 0 };
    char *buf;
    uint64_t copied = 0;
    int rc = 0;
    long n;

#if defined(_WIN32)
    in.h = INVALID_HANDLE_VALUE;
    out.h = INVALID_HANDLE_VALUE;
#else
    in.fd = -1;
    out.fd = -1;
#endif

    if (fs_open_read(src, &in) != 0) return -1;
    if (fs_open_write(dst, &out) != 0) {
        fs_close(&in);
        return -1;
    }
    buf = (char *)malloc(CHUNK);
    if (!buf) {
        fs_close(&in);
        fs_close(&out);
        return -1;
    }
    g.cur_size = size ? size : 1;
    g.cur_done = 0;

    while (!g.cancelled) {
        long off = 0;
        long w;
        n = fs_read(&in, buf, CHUNK);
        if (n <= 0) break;
        while (off < n) {
            w = fs_write(&out, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -1; break; }
            off += w;
        }
        if (rc) break;
        copied += (uint64_t)n;
        g.done_bytes += (uint64_t)n;
        g.cur_done = copied;
        g.sample_acc += (uint64_t)n;
        g.sample_n++;
        if (cancelled_poll()) break;
        maybe_sample();
        maybe_draw();
    }
    free(buf);
    fs_close(&in);
    fs_close(&out);
    return rc;
}

static void copy_tree(const char *src, const char *dst, char **ex, int nex)
{
    DirIter it;
    char *name = NULL;
    int isdir, islink;
    uint64_t size;
    char sp[PATHMAX * 2], dp[PATHMAX * 2];

    if (g.cancelled) return;

    if (!g.dry) {
        if (fs_exists(dst)) {
            if (!fs_isdir(dst)) {
                g.failed = 1;
                return;
            }
        } else if (fs_mkdir(dst) != 0 && !fs_isdir(dst)) {
            g.failed = 1;
            return;
        }
    }

    if (dir_open(src, &it) != 0) {
        g.failed = 1;
        return;
    }

    while (dir_next(&it, &name, &isdir, &size, &islink)) {
        int same = 0;
        if (g.cancelled || cancelled_poll()) { free(name); break; }
        if (excluded(name, ex, nex)) { free(name); continue; }

        snprintf(sp, sizeof sp, "%s/%s", src, name);
        snprintf(dp, sizeof dp, "%s/%s", dst, name);

        if (islink) {
            char tgt[PATHMAX];
            if (!g.dry && fs_readlink(sp, tgt, sizeof tgt) == 0) fs_symlink(tgt, dp);
            free(name);
            continue;
        }
        if (isdir) {
            copy_tree(sp, dp, ex, nex);
            free(name);
            continue;
        }

        snprintf(g.cur, sizeof g.cur, "%s", sp);
        {
            size_t root = strlen(src);
            const char *rel = (strncmp(sp, src, root) == 0 && sp[root] == '/') ? sp + root + 1 : sp;
            push_name(rel);
        }

        {
            uint64_t dsize = 0, ssize = 0;
            int64_t smt = 0, dmt = 0;
            if (fs_meta(dp, &dsize, &dmt) == 0 && fs_meta(sp, &ssize, &smt) == 0 &&
                dsize == ssize) {
                int64_t diff = dmt - smt;
                if (diff < 0) diff = -diff;
                if (diff <= MTIME_TOL) same = 1;
            }
        }

        if (same || g.dry) {
            g.done_bytes += size;
            g.done_files++;
            free(name);
            maybe_draw();
            continue;
        }

        if (copy_file(sp, dp, size) == 0) {
            uint64_t sz = 0;
            int64_t mt = 0;
            if (fs_meta(sp, &sz, &mt) == 0) fs_set_mtime(dp, mt);
            g.done_files++;
        } else {
            g.failed = 1;
        }
        free(name);
        maybe_draw();
    }
    dir_close(&it);
}

/* ----------------------------------------------------------------- main --- */

static void usage(void)
{
    printf("cpflow " VERSION " - copy with a btop-style terminal UI\n"
           "\n"
           "usage:\n"
           "  cpflow [options] SOURCE DEST\n"
           "\n"
           "options:\n"
           "  -n, --dry-run         show what would be copied, copy nothing\n"
           "  --exclude=PATTERN     skip names matching PATTERN (* and ? work)\n"
           "  -h, --help            show this help\n"
           "  -V, --version         show version\n"
           "\n"
           "keys:\n"
           "  q, Esc, Ctrl-C        stop copying and exit\n"
           "\n"
           "The contents of SOURCE are copied into DEST. Files that already exist\n"
           "with the same size and modification time are skipped, so re-running\n"
           "the same command resumes where it stopped.\n"
           "\n"
           "examples:\n"
           "  cpflow /media/user/OLD /media/user/NEW\n"
           "  cpflow --exclude=.Trash-1000 /media/user/OLD /media/user/NEW\n");
}

int main(int argc, char **argv)
{
    char *ex[MAX_EXCLUDES];
    int nex = 0, dry = 0, i;
    const char *src = NULL, *dst = NULL;

    memset(&g, 0, sizeof g);

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("cpflow " VERSION "\n");
            return 0;
        }
        if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) { dry = 1; continue; }
        if (!strncmp(a, "--exclude=", 10)) {
            if (nex < MAX_EXCLUDES) ex[nex++] = (char *)a + 10;
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "cpflow: unknown option %s (try --help)\n", a);
            return 2;
        }
        if (!src) src = a;
        else if (!dst) dst = a;
    }
    if (!src || !dst) {
        fprintf(stderr, "cpflow: expected SOURCE and DEST (try --help)\n");
        return 2;
    }
    if (!fs_exists(src) || !fs_isdir(src)) {
        fprintf(stderr, "cpflow: source is not a directory: %s\n", src);
        return 2;
    }
    if (!fs_isdir(dst)) {
        fprintf(stderr, "cpflow: destination is not a directory: %s\n", dst);
        return 2;
    }

    snprintf(g.src, sizeof g.src, "%s", src);
    snprintf(g.dst, sizeof g.dst, "%s", dst);
    g.dry = dry;
    g.t0 = now_sec();
    g.last_sample = g.t0;

#if defined(_WIN32)
    g.tty = _isatty(_fileno(stdout)) != 0;
#else
    g.tty = isatty(STDOUT_FILENO) != 0;
#endif
    if (g.tty) term_init();

    g.scanning = 1;
    if (g.tty) draw();
    else printf("cpflow: scanning %s …\n", src);
    scan_tree(src);
    g.scanning = 0;
    if (!g.tty) {
        char tot[32];
        human(g.total_bytes, tot, sizeof tot);
        printf("cpflow: %llu files, %s\n", (unsigned long long)g.total_files, tot);
    }

    if (!g.cancelled) {
        copy_tree(src, dst, ex, nex);
    }

    if (g.tty) {
        draw();
        term_done();
    }

    {
        char done[32], tot[32], clk[32];
        human(g.done_bytes, done, sizeof done);
        human(g.total_bytes, tot, sizeof tot);
        clock_str(elapsed(), clk, sizeof clk);
        if (g.cancelled) {
            printf("cancelled after %s - %s of %s copied; run again to resume\n", clk, done, tot);
            return 130;
        }
        if (g.failed) {
            printf("finished with errors in %s - %s of %s copied, %llu files\n", clk, done, tot,
                   (unsigned long long)g.done_files);
            return 1;
        }
        if (dry) {
            printf("dry run: %llu files, %s would be copied (%s)\n",
                   (unsigned long long)g.total_files, tot, clk);
            return 0;
        }
        printf("done in %s - %s, %llu files\n", clk, done, (unsigned long long)g.done_files);
    }
    return 0;
}
