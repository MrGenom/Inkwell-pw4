/* files: a file manager for Inkwell on the Kindle.
 *
 * Two modes, switched in the header:
 *   Device  the whole Kindle: the stock system as init (pid 1, outside our
 *           chroot) sees it, through /proc/1/root. The Kindle drive is
 *           /mnt/base-us there, our Linux image is mounted at /mnt/own.
 *   Linux   only our own Linux: the Alpine image we run in (chroot "/").
 *
 * A list of the current folder, folders first, with size and date. Tap a
 * folder to go in and the back chevron to go up; the ✕ at the top right (or a
 * long press on the chevron) leaves for the home screen at once. Tap a book (EPUB, PDF, MOBI,
 * FB2, CBZ...) to open it in Books, a text file to read it here. The "⋯" at
 * the end of a row, or a long press on it, opens the actions: open, rename,
 * copy, cut, delete. Copy and cut keep the item on a clipboard until Paste
 * in another folder (also across modes).
 *
 * Safety: read-only file systems and /proc, /sys, /dev only allow looking;
 * mount points and alpine.img can't be deleted, renamed or moved; deleting
 * anything outside the user's own places (the Kindle drive, /root, /home,
 * /tmp) asks with a system warning.
 *
 * The on-screen keyboard is Latin only, so new names are typed in Latin.
 */
#include "ui.h"
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>

#define BOOKS_APP "/usr/local/bin/books"
#define MAXENT 2000
#define PATHN 4096

enum { MODE_DEVICE, MODE_LINUX };
static const char *const mode_prefix[2] = {"/proc/1/root", ""};
static int mode = MODE_DEVICE;
static char cwds[2][PATHN] = {"/", "/"}; /* each mode remembers its folder */
#define cwd cwds[mode]                    /* shown path, inside the mode's root */

typedef struct {
    char name[256], target[256];
    int dir, link, broken, special; /* special: device, socket, pipe */
    long long size;
    time_t mtime;
} entry;
static entry ents[MAXENT];
static int nents, page, show_hidden, truncated;
static int here_ro, here_pseudo; /* the current folder: read-only / proc, sys, dev */
static char clip[PATHN];         /* real path picked by Copy / Cut, "" when nothing */
static int clip_cut;

/* ---- paths ------------------------------------------------------------------- */

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* dir + "/" + name, without doubling the slash after "/". */
static int child(char *out, int n, const char *dir, const char *name) {
    int len = strlen(dir);
    return snprintf(out, n, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", name) < n ? 0 : -1;
}

/* Collapses "//", "." and ".." in an absolute path, in place. */
static void normalize(char *p) {
    char tmp[PATHN], *parts[512];
    int np = 0;
    snprintf(tmp, sizeof tmp, "%s", p);
    for (char *s = strtok(tmp, "/"); s && np < 512; s = strtok(0, "/")) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (np) np--; continue; }
        parts[np++] = s;
    }
    char out[PATHN] = "";
    int k = 0;
    for (int i = 0; i < np && k < PATHN - 2; i++) k += snprintf(out + k, PATHN - k, "/%s", parts[i]);
    snprintf(p, PATHN, "%s", np ? out : "/");
}

/* The real path (as this process sees it) of a path shown in the current mode. */
static void real_of(const char *shown, char *out, int n) {
    snprintf(out, n, "%s%s", mode_prefix[mode], shown);
}

static int starts_with(const char *s, const char *pre) {
    size_t n = strlen(pre);
    return !strncmp(s, pre, n) && (s[n] == 0 || s[n] == '/');
}

/* Places that belong to the owner, where deleting needs no system warning. */
static int in_user_place(const char *real) {
    static const char *const ok[] = {"/mnt/us", "/proc/1/root/mnt/base-us", "/proc/1/root/mnt/us",
                                     "/root", "/home", "/tmp", "/proc/1/root/mnt/own/root", "/proc/1/root/mnt/own/home", 0};
    for (int i = 0; ok[i]; i++) if (starts_with(real, ok[i])) return 1;
    return 0;
}

/* proc, sysfs, devpts, or a tmpfs that is some /dev: only for looking. */
static int is_pseudo(const char *real) {
    struct statfs fs;
    if (statfs(real, &fs)) return 0;
    unsigned long t = (unsigned long)fs.f_type;
    if (t == 0x9fa0 || t == 0x62656572 || t == 0x1cd1) return 1;
    if (t == 0x01021994 && (strstr(real, "/dev/") || (strlen(real) >= 4 && !strcmp(real + strlen(real) - 4, "/dev")))) return 1;
    return 0;
}

static int is_readonly(const char *real) {
    struct statvfs vs;
    return !statvfs(real, &vs) && (vs.f_flag & ST_RDONLY);
}

/* A mount point (another file system starts here), alpine.img, or the root
 * of a mode: never deleted, renamed or moved from here. */
static int is_protected(const char *real) {
    if (!strcmp(base_name(real), "alpine.img")) return 1;
    char parent[PATHN];
    snprintf(parent, sizeof parent, "%s", real);
    char *s = strrchr(parent, '/');
    if (!s) return 1;
    if (s == parent) s[1] = 0; else *s = 0;
    struct stat a, b;
    if (lstat(real, &a) || stat(parent, &b)) return 0;
    return S_ISDIR(a.st_mode) && a.st_dev != b.st_dev;
}

/* ---- small helpers ---------------------------------------------------------- */

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d && d != name ? d + 1 : "";
}

static int ext_in(const char *name, const char *const *list) {
    const char *e = ext_of(name);
    for (; *list; list++) if (!strcasecmp(e, *list)) return 1;
    return 0;
}

static const char *const book_ext[] = {"epub", "pdf", "mobi", "azw", "azw3", "fb2", "cbz", "cbt", "xps", "djvu", 0};
static const char *const text_ext[] = {"txt", "md", "log", "conf", "cfg", "ini", "sh", "py", "json", "csv", "xml",
                                       "html", "htm", "c", "h", "yml", "yaml", "example", "calibre", 0};

static void human_size(long long b, char *out, int n) {
    if (b < 1024) snprintf(out, n, "%lld %s", b, "B");
    else if (b < 1024 * 1024) snprintf(out, n, "%.1f %s", b / 1024.0, "KB");
    else if (b < 1024LL * 1024 * 1024) snprintf(out, n, "%.1f %s", b / 1048576.0, "MB");
    else snprintf(out, n, "%.2f %s", b / 1073741824.0, "GB");
}

static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static void short_date(time_t t, char *out, int n) {
    struct tm tm = *localtime(&t);
    snprintf(out, n, "%d %s %d, %02d:%02d", tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
}

/* ---- reading a folder ------------------------------------------------------- */

static int by_kind_name(const void *a, const void *b) {
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

/* Where a symlink in the current folder points, as a path shown in this mode:
 * absolute targets are inside the mode's root, not inside our chroot. */
static void link_dest(const entry *t, char *out, int n) {
    if (t->target[0] == '/') snprintf(out, n, "%s", t->target);
    else { char tmp[PATHN]; child(tmp, sizeof tmp, cwd, t->target); snprintf(out, n, "%s", tmp); }
    normalize(out);
}

static void load_dir(void) {
    nents = 0;
    truncated = 0;
    char real[PATHN];
    real_of(cwd, real, sizeof real);
    here_ro = is_readonly(real);
    here_pseudo = is_pseudo(real);
    DIR *d = opendir(real);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!show_hidden && e->d_name[0] == '.') continue;
        if (nents >= MAXENT) { truncated = 1; break; }
        entry *t = &ents[nents];
        memset(t, 0, sizeof *t);
        char path[PATHN];
        if (child(path, sizeof path, real, e->d_name)) continue;
        struct stat st;
        if (lstat(path, &st)) continue;
        snprintf(t->name, sizeof t->name, "%s", e->d_name);
        t->mtime = st.st_mtime;
        t->size = st.st_size;
        if (S_ISLNK(st.st_mode)) {
            t->link = 1;
            ssize_t k = readlink(path, t->target, sizeof t->target - 1);
            t->target[k > 0 ? k : 0] = 0;
            char dest[PATHN], rdest[PATHN];
            link_dest(t, dest, sizeof dest);
            real_of(dest, rdest, sizeof rdest);
            struct stat ts;
            if (stat(rdest, &ts) == 0) {
                t->dir = S_ISDIR(ts.st_mode);
                t->special = !t->dir && !S_ISREG(ts.st_mode);
                t->size = ts.st_size;
            }
            else t->broken = 1;
        } else {
            t->dir = S_ISDIR(st.st_mode);
            t->special = !t->dir && !S_ISREG(st.st_mode);
        }
        nents++;
    }
    if (d) closedir(d);
    qsort(ents, nents, sizeof *ents, by_kind_name);
}

/* ---- copying and deleting --------------------------------------------------- */

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[PATHN];
        if (child(sub, sizeof sub, path, e->d_name) || remove_tree(sub)) bad = 1;
    }
    closedir(d);
    return rmdir(path) || bad ? -1 : 0;
}

static int copy_file(const char *src, const char *dst) {
    static char buf[1 << 16];
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    size_t n;
    int bad = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { bad = 1; break; }
    if (ferror(in)) bad = 1;
    fclose(in);
    if (fclose(out)) bad = 1;
    if (bad) unlink(dst);
    return bad ? -1 : 0;
}

/* Copies files and folders; symlinks are copied as symlinks, and other
 * special files (devices, sockets) are skipped. */
static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st)) return -1;
    if (S_ISLNK(st.st_mode)) {
        char t[PATHN];
        ssize_t k = readlink(src, t, sizeof t - 1);
        if (k < 0) return -1;
        t[k] = 0;
        return symlink(t, dst) ? copy_file(src, dst) : 0; /* FAT has no symlinks: copy the content */
    }
    if (S_ISREG(st.st_mode)) return copy_file(src, dst);
    if (!S_ISDIR(st.st_mode)) return 0;
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int bad = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[PATHN], t[PATHN];
        if (child(s, sizeof s, src, e->d_name) || child(t, sizeof t, dst, e->d_name) || copy_tree(s, t)) bad = 1;
    }
    closedir(d);
    return bad ? -1 : 0;
}

/* A name in dir that isn't taken yet: "name", then "name (2)", "name (3)"... */
static void free_name(const char *dir, const char *name, char *out, int n) {
    char stem[256], ext[64] = "";
    snprintf(stem, sizeof stem, "%s", name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) { snprintf(ext, sizeof ext, "%s", dot); *dot = 0; }
    struct stat st;
    child(out, n, dir, name);
    for (int i = 2; lstat(out, &st) == 0 && i < 1000; i++) {
        char nm[300];
        snprintf(nm, sizeof nm, "%s (%d)%s", stem, i, ext);
        child(out, n, dir, nm);
    }
}

/* ---- drawing ---------------------------------------------------------------- */

#define PATH_Y 168
#define TOOL_Y 214
#define TOOL_H 88
#define LIST_Y 330
#define ROW_H 118
#define FOOT_H 130
#define MORE_W 110
#define SEG_W 180
#define SEG_H 80

static rect tool_new, tool_paste, tool_cancel, tool_hidden, prev_btn, next_btn, seg_dev, seg_lin;
/* Quick exit: straight back to the home screen from any folder or the viewer. */
#define close_btn ((rect){W - 16 - 110, 28, 110, 110})

static int rows_fit(void) { return (H - FOOT_H - LIST_Y) / ROW_H; }
static int pages(void) { int f = rows_fit(); return nents ? (nents + f - 1) / f : 1; }
static rect row_rect(int i) { return (rect){MARGIN, LIST_Y + i * ROW_H, W - 2 * MARGIN, ROW_H}; }
static rect more_rect(int i) { rect r = row_rect(i); return (rect){r.x + r.w - MORE_W, r.y, MORE_W, r.h}; }
static int writable_here(void) { return !here_ro && !here_pseudo; }

static void icon_folder_small(float cx, float cy, float s) {
    float w = s * 0.78f, h = s * 0.58f, x = cx - w / 2, y = cy - h / 2 + s * 0.04f, t = s * 0.06f;
    float tab[] = {x, y - s * 0.12f, x + w * 0.38f, y - s * 0.12f, x + w * 0.48f, y, x, y};
    poly_fill(tab, 4, BLACK);
    rrect_line(x, y, w, h, s * 0.05f, t, BLACK);
}

/* A small pill with a 32 px label, for the mode switch. */
static void seg_pill(rect r, const char *label, int on) {
    if (on) rrect_fill(r.x, r.y, r.w, r.h, r.h / 2.0f, BLACK);
    else { rrect_fill(r.x, r.y, r.w, r.h, r.h / 2.0f, WHITE); rrect_line(r.x + 1.5f, r.y + 1.5f, r.w - 3, r.h - 3, r.h / 2.0f - 1.5f, 3, BLACK); }
    text_center(&f_ui_bold, 30, r.x + r.w / 2, r.y + (r.h - 30) / 2 - 3, label, on ? WHITE : BLACK);
}

/* The page header with a title that is cut to fit; with_modes adds the
 * Device / Linux switch on the right. */
static void title_bar(const char *title, int with_modes) {
    header("");
    float cx = close_btn.x + close_btn.w / 2.0f, cy = close_btn.y + close_btn.h / 2.0f, k = 22;
    line(cx - k, cy - k, cx + k, cy + k, 7, BLACK);
    line(cx - k, cy + k, cx + k, cy - k, 7, BLACK);
    int right = close_btn.x - 12;
    if (with_modes) {
        seg_lin = (rect){close_btn.x - 8 - SEG_W, 40, SEG_W, SEG_H};
        seg_dev = (rect){seg_lin.x - 12 - SEG_W, 40, SEG_W, SEG_H};
        seg_pill(seg_dev, "Device", mode == MODE_DEVICE);
        seg_pill(seg_lin, "Linux", mode == MODE_LINUX);
        right = seg_dev.x - 20;
    }
    char fit[300];
    text_fit(&f_serif_bold, 76, title, right - (back_btn.x + 110), fit, sizeof fit);
    text(&f_serif_bold, 76, back_btn.x + 110, 38, fit, BLACK);
}

static void draw_list(int flash) {
    const char *title = strcmp(cwd, "/") ? base_name(cwd) : mode == MODE_DEVICE ? "Device" : "Linux";
    title_bar(title, 1);
    char shown[PATHN + 32], fit[PATHN];
    snprintf(shown, sizeof shown, "%s:%s", mode == MODE_DEVICE ? "Kindle" : "Linux", cwd);
    text_fit(&f_ui, 30, shown, W - 2 * MARGIN, fit, sizeof fit);
    text(&f_ui, 30, MARGIN, PATH_Y, fit, 0x60);

    int gap = 14, bw = (W - 2 * MARGIN - 3 * gap) / 4, x = MARGIN;
    tool_new = tool_paste = tool_cancel = (rect){0, 0, 0, 0};
    if (writable_here()) {
        tool_new = (rect){x, TOOL_Y, bw, TOOL_H};
        pill(tool_new, "+ Folder", 0);
    } else {
        rrect_fill(x, TOOL_Y, bw, TOOL_H, TOOL_H / 2.0f, 0xE0);
        text_center(&f_ui_bold, 30, x + bw / 2, TOOL_Y + 27, here_pseudo ? "system" : "read-only", 0x50);
    }
    x += bw + gap;
    if (clip[0]) {
        if (writable_here()) {
            tool_paste = (rect){x, TOOL_Y, bw, TOOL_H};
            pill(tool_paste, clip_cut ? "Move here" : "Paste", 1);
        }
        x += bw + gap;
        tool_cancel = (rect){x, TOOL_Y, bw, TOOL_H};
        pill(tool_cancel, "Cancel", 0);
    }
    tool_hidden = (rect){W - MARGIN - bw, TOOL_Y, bw, TOOL_H};
    pill(tool_hidden, "Hidden", show_hidden);

    int fit_rows = rows_fit();
    if (page >= pages()) page = pages() - 1;
    if (!nents) {
        icon_folder_small(W / 2, 700, 240);
        text_center(&f_serif_italic, 58, W / 2, 860, "An empty folder", 0x30);
    }
    for (int i = 0; i < fit_rows; i++) {
        int k = page * fit_rows + i;
        if (k >= nents) break;
        entry *t = &ents[k];
        rect r = row_rect(i);
        float icx = r.x + 42, icy = r.y + r.h / 2.0f;
        if (t->dir) icon_folder_small(icx, icy, 78);
        else if (ext_in(t->name, book_ext)) icon_book(icx, icy, 84);
        else icon_page(icx, icy, 84);
        char nm[300], meta[400], sz[32], dt[48];
        int tx = r.x + 100, tw = r.w - 100 - MORE_W;
        text_fit(&f_ui_bold, 38, t->name, tw, nm, sizeof nm);
        text(&f_ui_bold, 38, tx, r.y + 16, nm, BLACK);
        short_date(t->mtime, dt, sizeof dt);
        if (t->link) snprintf(meta, sizeof meta, "→ %s%s", t->target, t->broken ? "  (missing)" : "");
        else if (t->dir) snprintf(meta, sizeof meta, "%s · %s", "folder", dt);
        else if (t->special) snprintf(meta, sizeof meta, "%s · %s", "special file", dt);
        else { human_size(t->size, sz, sizeof sz); snprintf(meta, sizeof meta, "%s · %s", sz, dt); }
        char mfit[400];
        text_fit(&f_ui, 28, meta, tw, mfit, sizeof mfit);
        text(&f_ui, 28, tx, r.y + 70, mfit, 0x60);
        rect m = more_rect(i);
        for (int j = -1; j <= 1; j++) disc(m.x + m.w / 2.0f + j * 18, m.y + m.h / 2.0f, 5.5f, BLACK);
        fill(r.x, r.y + r.h - 2, r.w, 2, 0xC8);
    }

    int fy = H - FOOT_H + 24;
    prev_btn = next_btn = (rect){0, 0, 0, 0};
    if (page > 0) { prev_btn = (rect){MARGIN, fy, 170, 84}; pill(prev_btn, "‹", 0); }
    if (page < pages() - 1) { next_btn = (rect){W - MARGIN - 170, fy, 170, 84}; pill(next_btn, "›", 0); }
    char foot[200], fr[32] = "?", real[PATHN];
    real_of(cwd, real, sizeof real);
    struct statvfs vs;
    if (!statvfs(real, &vs)) human_size((long long)vs.f_bavail * vs.f_frsize, fr, sizeof fr);
    if (pages() > 1)
        snprintf(foot, sizeof foot, "%d / %d  ·  %s %s%s", page + 1, pages(), fr, "free",
                 truncated ? "  ·  first 2000" : "");
    else snprintf(foot, sizeof foot, "%d %s  ·  %s %s", nents, nents == 1 ? "item" : "items", fr, "free");
    text_center(&f_ui, 30, W / 2, fy + 24, foot, 0x50);
    if (flash) flip_flash(); else flip();
}

/* ---- the action sheet -------------------------------------------------------- */

/* A card of buttons over the page. Returns the index tapped, or -1 for a tap
 * outside or the power button. The caller redraws the page afterwards. */
static int sheet(const char *title, const char *const *opts, int n) {
    int bh = 104, gap = 16, pad = 36, th = title ? 90 : 0;
    int w = W - 2 * MARGIN, h = pad * 2 + th + n * bh + (n - 1) * gap, x = MARGIN, y = H - h - 40;
    rrect_fill(x + 8, y + 10, w, h, 30, 0x80);
    rrect_fill(x, y, w, h, 30, WHITE);
    rrect_line(x + 2, y + 2, w - 4, h - 4, 28, 4, BLACK);
    if (title) {
        char fit[300];
        text_fit(&f_ui_bold, 36, title, w - 2 * pad, fit, sizeof fit);
        text_center(&f_ui_bold, 36, W / 2, y + pad + 4, fit, 0x30);
    }
    rect b[8];
    for (int i = 0; i < n && i < 8; i++) {
        b[i] = (rect){x + pad, y + pad + th + i * (bh + gap), w - 2 * pad, bh};
        pill(b[i], opts[i], i == 0);
    }
    refresh(x, y, w + 12, h + 14, WF_GC16, 0, 0);
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) return -1;
        if (e.type == EV_DOWN) {
            for (int i = 0; i < n; i++) if (hit(b[i], e.x, e.y)) { press_on(b[i]); break; }
            continue;
        }
        if (e.type != EV_TAP) continue;
        press_forget();
        for (int i = 0; i < n; i++) if (hit(b[i], e.x, e.y)) return i;
        if (!hit((rect){x, y, w, h}, e.x, e.y)) return -1;
    }
}

static void message(const char *msg) {
    toast(msg);
    sleep(2);
}

/* ---- opening files ----------------------------------------------------------- */

static int run(const char *prog, const char *arg1, const char *arg2) {
    int st = 0;
    wait_last_update();
    pid_t pid = fork();
    if (pid == 0) {
        execl(prog, base_name(prog), arg1, arg2, (char *)0);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &st, 0);
    input_drain();
    screen_taken_back();
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* A plain reader for text files: pages you turn by tapping the right or left
 * half of the screen, or by swiping. */
#define TV_PX 34
#define TV_LINE 48
#define TV_MAX (512 * 1024)
#define TV_LINES 40000

static char *tv_buf;
static int *tv_off, tv_n;

static void tv_emit(const char *line, int n, void *ctx) {
    (void)n; (void)ctx;
    if (tv_n < TV_LINES) tv_off[tv_n++] = (int)(line - tv_buf);
}

static void view_text(const char *path, const char *name) {
    FILE *f = fopen(path, "rb");
    if (!f) { message("Couldn't open the file"); return; }
    tv_buf = malloc(TV_MAX + 1);
    tv_off = malloc(sizeof(int) * TV_LINES);
    if (!tv_buf || !tv_off) { fclose(f); free(tv_buf); free(tv_off); message("Not enough memory"); return; }
    int len = fread(tv_buf, 1, TV_MAX, f), cut = !feof(f);
    fclose(f);
    tv_buf[len] = 0;
    for (int i = 0; i < len; i++) {
        if (tv_buf[i] == '\t') tv_buf[i] = ' ';
        else if (tv_buf[i] == '\r' || (tv_buf[i] > 0 && tv_buf[i] < 32 && tv_buf[i] != '\n') || tv_buf[i] == 0) tv_buf[i] = ' ';
    }
    tv_n = 0;
    text_wrap(&f_mono, TV_PX, tv_buf, W - 2 * MARGIN, tv_emit, 0);
    int top = HEADER_H + 30, per = (H - top - 90) / TV_LINE, pg = 0, npg = tv_n ? (tv_n + per - 1) / per : 1;
    for (;;) {
        title_bar(name, 0);
        for (int i = 0; i < per && pg * per + i < tv_n; i++) {
            int k = pg * per + i, s = tv_off[k];
            int e = k + 1 < tv_n ? tv_off[k + 1] : len;
            char line[1024];
            int n = e - s < (int)sizeof line - 1 ? e - s : (int)sizeof line - 1;
            memcpy(line, tv_buf + s, n);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == ' ')) n--;
            line[n] = 0;
            text(&f_mono, TV_PX, MARGIN, top + i * TV_LINE, line, BLACK);
        }
        char foot[128];
        snprintf(foot, sizeof foot, "%d / %d%s   ·   %s", pg + 1, npg,
                 cut ? "  ·  first 512 KB" : "",
                 "tap right / left to turn");
        text_center(&f_ui, 28, W / 2, H - 70, foot, 0x60);
        flip();
        for (;;) {
            event e = input_wait(-1);
            if (e.type == EV_POWER) { sleep_now(); break; }
            if (e.type == EV_DOWN) {
                if (hit(back_btn, e.x, e.y)) press_on(back_btn);
                else if (hit(close_btn, e.x, e.y)) press_on(close_btn);
                continue;
            }
            if (e.type != EV_TAP) continue;
            press_forget();
            if (hit(back_btn, e.x, e.y)) { free(tv_buf); free(tv_off); return; }
            if (hit(close_btn, e.x, e.y)) exit(0);
            int dx = e.ux - e.x, dy = e.uy - e.y;
            if (abs(dy) > 150 && abs(dy) > abs(dx)) pg += dy < 0 ? 1 : -1;
            else if (abs(dx) > 150) pg += dx < 0 ? 1 : -1;
            else pg += e.x > W / 2 ? 1 : -1;
            if (pg < 0) pg = 0;
            if (pg >= npg) pg = npg - 1;
            break;
        }
    }
}

static void enter_dir(const char *shown) {
    snprintf(cwd, PATHN, "%s", shown);
    normalize(cwd);
    page = 0;
    load_dir();
}

static void open_entry(entry *t) {
    char shown[PATHN], real[PATHN];
    if (t->link) link_dest(t, shown, sizeof shown);
    else if (child(shown, sizeof shown, cwd, t->name)) return;
    real_of(shown, real, sizeof real);
    if (t->broken) { message("The link points nowhere"); return; }
    if (t->dir) { enter_dir(shown); return; }
    if (t->special) { message("A device or special file: it can't be opened"); return; }
    if (ext_in(t->name, book_ext)) {
        if (access(BOOKS_APP, X_OK)) { message("The Books app is missing"); return; }
        run(BOOKS_APP, real, t->name);
        return;
    }
    if (ext_in(t->name, text_ext) || t->size < 64 * 1024) {
        /* small files of unknown type are usually text too (and /proc and
         * /sys files report size 0); binary ones just show as noise */
        view_text(real, t->name);
        return;
    }
    char sz[32], msg[200];
    human_size(t->size, sz, sizeof sz);
    snprintf(msg, sizeof msg, "%s (%s)", "No app opens this file", sz);
    message(msg);
}

/* ---- actions ----------------------------------------------------------------- */

static int valid_name(const char *s) {
    if (!s[0] || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (; *s; s++) if (*s == '/') return 0;
    return 1;
}

static void new_folder(void) {
    char name[256] = "", real[PATHN], path[PATHN];
    kb_enter_label = "Create";
    if (!text_entry("New folder", "Name:", name, sizeof name, 0)) return;
    real_of(cwd, real, sizeof real);
    if (!valid_name(name) || child(path, sizeof path, real, name)) { message("That name can't be used"); return; }
    if (mkdir(path, 0755)) {
        message(errno == EEXIST ? "That name is taken" : "Couldn't create it");
        return;
    }
    sync();
}

static void rename_entry(entry *t) {
    char name[256], real[PATHN], from[PATHN], to[PATHN];
    snprintf(name, sizeof name, "%s", t->name);
    kb_enter_label = "Done";
    if (!text_entry("Rename", "New name:", name, sizeof name, 0)) return;
    if (!strcmp(name, t->name)) return;
    real_of(cwd, real, sizeof real);
    struct stat st;
    if (!valid_name(name) || child(from, sizeof from, real, t->name) || child(to, sizeof to, real, name)) {
        message("That name can't be used");
        return;
    }
    if (lstat(to, &st) == 0) { message("That name is taken"); return; }
    if (rename(from, to)) { message("Couldn't rename it"); return; }
    sync();
}

static void delete_entry(entry *t) {
    char real[PATHN], path[PATHN], q[400];
    real_of(cwd, real, sizeof real);
    if (child(path, sizeof path, real, t->name)) return;
    int sys = !in_user_place(path);
    snprintf(q, sizeof q, "%sDelete \"%s\"%s?", sys ? "System file! " : "", t->name, t->dir && !t->link ? " and everything in it" : "");
    const char *const opts[] = {"Delete", "Cancel"};
    draw_list(0); /* clear the actions sheet first */
    if (sheet(q, opts, 2) != 0) return;
    if (sys) {
        draw_list(0);
        const char *const sure[] = {"Yes, delete it", "Cancel"};
        if (sheet("This can break the Kindle or Linux. Sure?", sure, 2) != 0) return;
    }
    if (!strcmp(path, clip)) clip[0] = 0;
    toast("Deleting...");
    int bad = remove_tree(path);
    sync();
    if (bad) message("Some of it couldn't be deleted");
}

static void paste(void) {
    if (!clip[0]) return;
    struct stat st;
    if (lstat(clip, &st)) { message("It no longer exists"); clip[0] = 0; return; }
    char real[PATHN], dst[PATHN];
    real_of(cwd, real, sizeof real);
    free_name(real, base_name(clip), dst, sizeof dst);
    size_t n = strlen(clip);
    if (S_ISDIR(st.st_mode) && !strncmp(dst, clip, n) && dst[n] == '/') {
        message("A folder can't go inside itself");
        return;
    }
    int bad;
    if (clip_cut) {
        char parent[PATHN];
        snprintf(parent, sizeof parent, "%s", clip);
        *strrchr(parent, '/') = 0;
        if (!strcmp(parent, real)) { clip[0] = 0; return; } /* already here */
        toast("Moving...");
        bad = rename(clip, dst);
        if (bad && errno == EXDEV) bad = copy_tree(clip, dst) || remove_tree(clip);
    } else {
        toast("Copying...");
        bad = copy_tree(clip, dst);
    }
    sync();
    if (bad) message(clip_cut ? "Couldn't move it" : "Couldn't copy it (drive full?)");
    else clip[0] = 0;
}

/* The actions for one entry: only those that are safe where it is. */
enum { A_OPEN, A_RENAME, A_COPY, A_CUT, A_DELETE, A_CANCEL };

static void entry_actions(entry *t) {
    char real[PATHN], path[PATHN];
    real_of(cwd, real, sizeof real);
    child(path, sizeof path, real, t->name);
    int can_change = writable_here() && !is_protected(path);
    const char *opts[6];
    int ids[6], n = 0;
    opts[n] = "Open"; ids[n++] = A_OPEN;
    if (can_change) { opts[n] = "Rename"; ids[n++] = A_RENAME; }
    if (!here_pseudo) { opts[n] = "Copy"; ids[n++] = A_COPY; }
    if (can_change) { opts[n] = "Cut"; ids[n++] = A_CUT; opts[n] = "Delete"; ids[n++] = A_DELETE; }
    opts[n] = "Cancel"; ids[n++] = A_CANCEL;
    char title[300];
    if (!can_change && writable_here()) snprintf(title, sizeof title, "%s  ·  %s", t->name, "protected");
    else snprintf(title, sizeof title, "%s", t->name);
    int c = sheet(title, opts, n);
    if (c < 0) return;
    switch (ids[c]) {
    case A_OPEN: open_entry(t); break;
    case A_RENAME: rename_entry(t); break;
    case A_COPY: case A_CUT: snprintf(clip, sizeof clip, "%s", path); clip_cut = ids[c] == A_CUT; break;
    case A_DELETE: delete_entry(t); break;
    default: break;
    }
}

/* ---- main loop ---------------------------------------------------------------- */

static void go_up(void) {
    char up[PATHN];
    snprintf(up, sizeof up, "%s/..", cwd);
    enter_dir(up);
}

int main(int argc, char **argv) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    /* files [device|linux] [folder] */
    if (argc > 1 && !strcmp(argv[1], "linux")) mode = MODE_LINUX;
    if (argc > 2) enter_dir(argv[2]);
    load_dir();
    draw_list(1);
    long down_ms = 0;
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) { sleep_now(); draw_list(1); continue; }
        int fit = rows_fit();
        if (e.type == EV_DOWN) {
            down_ms = mono_ms();
            if (hit(back_btn, e.x, e.y)) press_on(back_btn);
            else if (hit(close_btn, e.x, e.y)) press_on(close_btn);
            else if (hit(seg_dev, e.x, e.y)) press_on(seg_dev);
            else if (hit(seg_lin, e.x, e.y)) press_on(seg_lin);
            else if (tool_new.w && hit(tool_new, e.x, e.y)) press_on(tool_new);
            else if (tool_paste.w && hit(tool_paste, e.x, e.y)) press_on(tool_paste);
            else if (tool_cancel.w && hit(tool_cancel, e.x, e.y)) press_on(tool_cancel);
            else if (hit(tool_hidden, e.x, e.y)) press_on(tool_hidden);
            else if (prev_btn.w && hit(prev_btn, e.x, e.y)) press_on(prev_btn);
            else if (next_btn.w && hit(next_btn, e.x, e.y)) press_on(next_btn);
            else for (int i = 0; i < fit && page * fit + i < nents; i++) if (hit(row_rect(i), e.x, e.y)) { press_on(row_rect(i)); break; }
            continue;
        }
        if (e.type != EV_TAP) continue;
        press_forget();
        int longp = mono_ms() - down_ms > 650;
        int dx = e.ux - e.x, dy = e.uy - e.y;
        if (abs(dy) > 180 && abs(dy) > abs(dx) && e.y > LIST_Y) { /* swipe pages */
            if (dy < 0 && page < pages() - 1) page++;
            else if (dy > 0 && page > 0) page--;
            draw_list(0);
            continue;
        }
        if (hit(close_btn, e.x, e.y)) return 0;
        if (hit(back_btn, e.x, e.y)) {
            if (longp || !strcmp(cwd, "/")) return 0;
            go_up();
        } else if (hit(seg_dev, e.x, e.y) || hit(seg_lin, e.x, e.y)) {
            int m = hit(seg_dev, e.x, e.y) ? MODE_DEVICE : MODE_LINUX;
            if (m != mode) { mode = m; page = 0; load_dir(); }
        } else if (tool_new.w && hit(tool_new, e.x, e.y)) { new_folder(); load_dir(); }
        else if (tool_paste.w && hit(tool_paste, e.x, e.y)) { paste(); load_dir(); }
        else if (tool_cancel.w && hit(tool_cancel, e.x, e.y)) clip[0] = 0;
        else if (hit(tool_hidden, e.x, e.y)) { show_hidden = !show_hidden; page = 0; load_dir(); }
        else if (prev_btn.w && hit(prev_btn, e.x, e.y)) page--;
        else if (next_btn.w && hit(next_btn, e.x, e.y)) page++;
        else {
            int done = 0;
            for (int i = 0; i < fit && page * fit + i < nents; i++) {
                if (!hit(row_rect(i), e.x, e.y)) continue;
                entry t = ents[page * fit + i]; /* load_dir below may reuse the slot */
                char before[PATHN];
                snprintf(before, sizeof before, "%s", cwd);
                if (longp || hit(more_rect(i), e.x, e.y)) entry_actions(&t);
                else open_entry(&t);
                if (!strcmp(before, cwd)) load_dir(); /* entering a folder already loaded it */
                done = 1;
                break;
            }
            if (!done) { press_off(); continue; }
        }
        draw_list(0);
    }
}
