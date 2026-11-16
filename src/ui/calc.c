/* calc: a calculator. The sum is typed on big keys and worked out as on
 * paper, so 2 + 3 × 4 is 14; it has brackets, powers, square roots and
 * percent (200 + 10% is 220). The answer shows in grey under the sum while
 * you type, and = makes it the start of the next sum, with the old one in
 * small type above. */
#include "ui.h"
#include <math.h>

#define MAXE 80
/* The sum, in one byte per token: digits, '.', + - * / ^ % ( ) and r for a
 * square root, s for squared. pretty() turns it into what the screen shows. */
static char ex[MAXE + 1];
static char prev[3 * MAXE + 16]; /* the last sum and its "=", shown small */
static int answered;            /* ex holds the answer of = */
static const char *failed;      /* why = could not work the sum out */

/* ---- working it out ---------------------------------------------------- */

static const char *pp;
static int perr, div0, pct_end, term_pct;
static double p_expr(void);
static double p_unary(void);

static double p_primary(void) {
    if (*pp == '(') {
        pp++;
        double v = p_expr();
        if (*pp == ')') pp++; else perr = 1;
        return v;
    }
    if (*pp == 'r') { pp++; return sqrt(p_unary()); }
    if ((*pp >= '0' && *pp <= '9') || *pp == '.') {
        char *end;
        double v = strtod(pp, &end);
        if (end == pp) perr = 1;
        pp = end;
        return v;
    }
    perr = 1;
    return 0;
}

static double p_postfix(void) {
    double v = p_primary();
    pct_end = 0;
    for (;;) {
        if (*pp == '%') { pp++; v /= 100; pct_end = 1; }
        else if (*pp == 's') { pp++; v *= v; pct_end = 0; }
        else return v;
    }
}

/* Powers bind right to left and above a leading minus: -2^2 is -4. */
static double p_power(void) {
    double v = p_postfix();
    if (*pp == '^') { pp++; v = pow(v, p_unary()); pct_end = 0; }
    return v;
}

static double p_unary(void) {
    if (*pp == '-') { pp++; return -p_unary(); }
    if (*pp == '+') { pp++; return p_unary(); }
    return p_power();
}

/* A number, bracket or root straight after another is a product: 2(3+4),
 * 2√9, (1+1)(2+2). */
static double p_term(void) {
    double v = p_unary();
    int single = 1, pct = pct_end;
    for (;;) {
        char c = *pp;
        if (c == '*' || c == '/') {
            pp++;
            double d = p_unary();
            if (c == '/' && d == 0) div0 = 1;
            v = c == '*' ? v * d : v / d;
        } else if (c == '(' || c == 'r' || (c >= '0' && c <= '9') || c == '.') v *= p_unary();
        else break;
        single = 0;
    }
    term_pct = single && pct;
    return v;
}

/* a + b% adds b percent of a, as calculators do. */
static double p_expr(void) {
    double v = p_term();
    while (*pp == '+' || *pp == '-') {
        char op = *pp++;
        double t = p_term();
        if (term_pct) t *= v;
        v = op == '+' ? v + t : v - t;
    }
    return v;
}

/* Works out the sum, closing any brackets left open. 0 if it can't. */
static int evaluate(const char *s, double *out) {
    char buf[MAXE + 40];
    int open = 0, n = 0;
    for (const char *c = s; *c; c++) open += (*c == '(') - (*c == ')');
    n = snprintf(buf, sizeof buf, "%s", s);
    while (open-- > 0 && n < (int)sizeof buf - 1) buf[n++] = ')', buf[n] = 0;
    if (!buf[0]) return 0;
    pp = buf;
    perr = div0 = 0;
    double v = p_expr();
    if (perr || *pp || div0 || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* The answer with at most 12 significant digits, which hides float noise
 * (0.1 + 0.2 is 0.3). */
static void number(double v, char *out, int n) {
    if (fabs(v) < 1e-12) v = 0;
    snprintf(out, n, "%.12g", v);
}

static int is_op(char c) { return c && strchr("+-*/^", c); }

/* The sum as the screen shows it: × ÷ − √ ², with room around operators. */
static void pretty(const char *s, char *out, int n) {
    int o = 0;
    out[0] = 0;
    for (int i = 0; s[i] && o < n - 8; i++) {
        char c = s[i], p = i ? s[i - 1] : 0;
        const char *r;
        char one[2] = {c, 0};
        int unary = c == '-' && (!p || is_op(p) || p == '(' || p == 'r' || p == 'e');
        if (p == 'e' && (c == '+' || c == '-')) r = c == '-' ? "−" : "+";
        else if (unary) r = "−";
        else if (c == '+') r = " + ";
        else if (c == '-') r = " − ";
        else if (c == '*') r = " × ";
        else if (c == '/') r = " ÷ ";
        else if (c == 'r') r = "√";
        else if (c == 's') r = "²";
        else r = one;
        o += snprintf(out + o, n - o, "%s", r);
    }
}

/* ---- typing ------------------------------------------------------------- */

static int ends_value(char c) { return (c >= '0' && c <= '9') || c == '.' || c == ')' || c == '%' || c == 's'; }
static char last(void) { int n = strlen(ex); return n ? ex[n - 1] : 0; }
static void append(const char *s) { if (strlen(ex) + strlen(s) <= MAXE) strcat(ex, s); }

/* Where the number at the end of the sum starts, or -1. */
static int last_number(void) {
    int i = strlen(ex);
    while (i > 0 && ((ex[i - 1] >= '0' && ex[i - 1] <= '9') || ex[i - 1] == '.' || ex[i - 1] == 'e' ||
                     ((ex[i - 1] == '+' || ex[i - 1] == '-') && i > 1 && ex[i - 2] == 'e')))
        i--;
    return i < (int)strlen(ex) ? i : -1;
}

static void key_press(const char *k) {
    char c = k[0];
    failed = 0;
    if (!strcmp(k, "C")) { ex[0] = prev[0] = 0; answered = 0; return; }
    if (!strcmp(k, "del")) { int n = strlen(ex); if (n) ex[n - 1] = 0; answered = 0; return; }
    if (!strcmp(k, "=")) {
        double v;
        if (!ex[0]) return;
        if (!evaluate(ex, &v)) { failed = div0 ? "Can't divide by zero" : "Can't work this out"; return; }
        char p[sizeof prev];
        pretty(ex, p, sizeof p - 4);
        snprintf(prev, sizeof prev, "%s =", p);
        number(v, ex, sizeof ex);
        answered = 1;
        return;
    }
    if (!strcmp(k, "neg")) {
        int i = last_number();
        if (i < 0) { if (!ends_value(last())) append("-"); return; }
        if (i > 0 && ex[i - 1] == '-' && (i == 1 || is_op(ex[i - 2]) || ex[i - 2] == '(' || ex[i - 2] == 'r'))
            memmove(ex + i - 1, ex + i, strlen(ex + i) + 1);
        else if (strlen(ex) < MAXE) { memmove(ex + i + 1, ex + i, strlen(ex + i) + 1); ex[i] = '-'; }
        answered = 0;
        return;
    }
    /* After =, a digit starts a new sum and an operator carries on. */
    if (answered && ((c >= '0' && c <= '9') || c == '.' || c == '(' || c == 'r')) ex[0] = 0;
    answered = 0;
    char l = last();
    if ((c >= '0' && c <= '9')) {
        if (l == ')' || l == '%' || l == 's') append("*");
        append(k);
    } else if (c == '.') {
        int i = last_number();
        if (i >= 0 && strchr(ex + i, '.')) return;
        if (i < 0) append(ends_value(l) ? "*0" : "0");
        append(".");
    } else if (is_op(c)) {
        if (!l) { if (c == '-') append("-"); return; }
        if (l == '(' || l == 'r') { if (c == '-') append("-"); return; }
        if (is_op(l)) {
            /* a minus after × ÷ or ^ is the sign of the next number */
            if (c == '-' && (l == '*' || l == '/' || l == '^')) { append("-"); return; }
            int n = strlen(ex);
            while (n > 0 && is_op(ex[n - 1])) n--;
            if (!n) { if (c == '-') { ex[0] = '-'; ex[1] = 0; } return; }
            ex[n] = 0;
        }
        append(k);
    } else if (c == '(' || c == 'r') {
        if (l == '.') return;
        append(k);
    } else if (c == ')') {
        int open = 0;
        for (const char *p = ex; *p; p++) open += (*p == '(') - (*p == ')');
        if (open > 0 && ends_value(l)) append(")");
    } else if (c == '%' || c == 's') {
        if (ends_value(l) && l != '.') append(k);
    }
}

/* ---- the page ---------------------------------------------------------- */

#define COLS 5
#define ROWS 5
#define CKEY_GAP 18
#define CKEY_H 150
#define PAD_Y (H - 44 - ROWS * CKEY_H - (ROWS - 1) * CKEY_GAP)

enum { K_DIGIT, K_OP, K_FN, K_EQ };
typedef struct { const char *code, *label; int kind; rect r; } calc_key;
static calc_key keys[ROWS * COLS] = {
    {"C", "C", K_FN},   {"(", "(", K_FN},   {")", ")", K_FN},   {"%", "%", K_FN},     {"/", "÷", K_OP},
    {"r", "√", K_FN},   {"7", "7", K_DIGIT}, {"8", "8", K_DIGIT}, {"9", "9", K_DIGIT},  {"*", "×", K_OP},
    {"s", "x2", K_FN},  {"4", "4", K_DIGIT}, {"5", "5", K_DIGIT}, {"6", "6", K_DIGIT},  {"-", "−", K_OP},
    {"^", "xy", K_FN},  {"1", "1", K_DIGIT}, {"2", "2", K_DIGIT}, {"3", "3", K_DIGIT},  {"+", "+", K_OP},
    {"neg", "±", K_FN}, {"0", "0", K_DIGIT}, {".", ".", K_DIGIT}, {"del", "⌫", K_FN},  {"=", "=", K_EQ},
};

static void layout(void) {
    int kw = (W - 2 * MARGIN - (COLS - 1) * CKEY_GAP) / COLS;
    for (int i = 0; i < ROWS * COLS; i++)
        keys[i].r = (rect){MARGIN + (i % COLS) * (kw + CKEY_GAP), PAD_Y + (i / COLS) * (CKEY_H + CKEY_GAP), kw, CKEY_H};
}

static void draw_key(const calc_key *k) {
    rect r = k->r;
    int ink = k->kind == K_EQ ? WHITE : BLACK;
    if (k->kind == K_EQ) rrect_fill(r.x, r.y, r.w, r.h, 30, BLACK);
    else {
        rrect_fill(r.x, r.y, r.w, r.h, 30, WHITE);
        float t = k->kind == K_OP ? 5 : k->kind == K_FN ? 2 : 3;
        rrect_line(r.x + t / 2, r.y + t / 2, r.w - t, r.h - t, 30 - t / 2, t, BLACK);
    }
    int cx = r.x + r.w / 2;
    if (!strcmp(k->label, "x2") || !strcmp(k->label, "xy")) { /* x with a raised 2 or y */
        int px = 58, w = text_width(&f_ui, px, "x"), sw = text_width(&f_ui, 36, k->label + 1);
        int x = cx - (w + sw) / 2, y = r.y + (r.h - px) / 2 - 4;
        text(&f_ui, px, x, y, "x", BLACK);
        text(&f_ui, 36, x + w + 2, y - 8, k->label + 1, BLACK);
        return;
    }
    font *f = k->kind == K_DIGIT ? &f_ui : &f_ui_bold;
    /* Inter draws × ÷ − + √ ± and ⌫ small next to its digits. */
    int px = k->kind == K_DIGIT ? 76 : k->kind != K_FN ? 96 : strchr("C(%)", k->label[0]) ? 64 : 76;
    text_center(f, px, cx, r.y + (r.h - px) / 2 - px / 8, k->label, ink);
}

#define DISP_Y (HEADER_H + 10)
#define DISP_H (PAD_Y - 30 - DISP_Y)

/* The sum, as large as fits: down to 56 px, then its start gives way to an
 * ellipsis. */
static void draw_sum(const char *s, int y_bottom) {
    int px = 104, max_w = W - 2 * MARGIN;
    while (px > 56 && text_width(&f_ui, px, s) > max_w) px -= 8;
    const char *t = s;
    char buf[sizeof prev + 8];
    if (text_width(&f_ui, px, t) > max_w) {
        while (*t && text_width(&f_ui, px, t) + text_width(&f_ui, px, "…") > max_w) {
            t++;
            while ((*t & 0xC0) == 0x80) t++;
        }
        snprintf(buf, sizeof buf, "…%s", t);
        t = buf;
    }
    text_right(&f_ui, px, W - MARGIN, y_bottom - px, t, BLACK);
}

static void draw_display(void) {
    fill(0, DISP_Y, W, DISP_H, WHITE);
    char line[sizeof prev], fit[sizeof prev];
    if (prev[0]) {
        text_fit(&f_ui, 44, prev, W - 2 * MARGIN, fit, sizeof fit);
        text_right(&f_ui, 44, W - MARGIN, DISP_Y + 24, fit, 0x60);
    }
    pretty(ex, line, sizeof line);
    draw_sum(ex[0] ? line : "0", DISP_Y + 250);
    double v;
    if (failed) text_right(&f_ui_bold, 52, W - MARGIN, DISP_Y + 290, failed, BLACK);
    else if (!answered && ex[0] && strpbrk(ex, "+-*/^%()rs") && evaluate(ex, &v)) {
        char num[40], shown[64];
        number(v, num, sizeof num);
        snprintf(shown, sizeof shown, "= %s", num);
        pretty(shown, line, sizeof line);
        text_fit(&f_ui, 60, line, W - 2 * MARGIN, fit, sizeof fit);
        text_right(&f_ui, 60, W - MARGIN, DISP_Y + 290, fit, 0x60);
    }
}

static void draw(void) {
    header("Calculator");
    draw_display();
    fill(MARGIN, PAD_Y - 30, W - 2 * MARGIN, 2, BLACK);
    for (int i = 0; i < ROWS * COLS; i++) draw_key(&keys[i]);
    flip();
}

/* Only the display changes per key. Every 24th time it gets a full refresh,
 * which clears what the fast ones leave behind. */
static void show(void) {
    static int n;
    draw_display();
    if (++n % 24 == 0) refresh(0, DISP_Y, W, DISP_H, WF_GC16, 1, 0);
    else refresh(0, DISP_Y, W, DISP_H, WF_GL16, 0, 0);
}

int main(void) {
    if (fb_open() || fonts_load()) return 1;
    input_open();
    layout();
    draw();
    for (;;) {
        event e = input_wait(-1);
        if (e.type == EV_POWER) return 0;
        if (e.type == EV_DOWN) {
            if (hit(back_btn, e.x, e.y)) { press_on(back_btn); continue; }
            for (int i = 0; i < ROWS * COLS; i++)
                if (hit(keys[i].r, e.x, e.y)) { press_on(keys[i].r); break; }
            continue;
        }
        if (e.type != EV_TAP) continue;
        if (hit(back_btn, e.x, e.y)) { press_forget(); return 0; }
        press_off();
        for (int i = 0; i < ROWS * COLS; i++)
            if (hit(keys[i].r, e.x, e.y)) { key_press(keys[i].code); show(); break; }
    }
}
