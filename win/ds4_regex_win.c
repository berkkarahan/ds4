/* Extended regular expressions for the Windows agent grep tool.
 * A backtracking compiler: concatenations fall through, and '|' / quantifiers
 * patch JMP and SPLIT holes once their continuation exists. */
#include "ds4_regex_win.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_CHAR = 1,
    OP_ANY,
    OP_CLASS,
    OP_BOL,
    OP_EOL,
    OP_SPLIT,
    OP_JMP,
    OP_MATCH
};

struct ds4_re_inst {
    int op;
    int x;
    int y;
    unsigned char cls[32];
};

typedef struct {
    struct ds4_re_inst *code;
    int n, cap;
    struct {
        int inst;
        int field;
        int next;
    } holes[2048];
    int nh;
    int icase;
    int failed;
} Prog;

typedef struct {
    int start;
    int out;
    int fall;
} Frag;

static int emit(Prog *p, struct ds4_re_inst in)
{
    if (p->failed) return -1;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 32;
        struct ds4_re_inst *code = (struct ds4_re_inst *)realloc(
            p->code, (size_t)cap * sizeof(*code));
        if (!code) {
            p->failed = 1;
            return -1;
        }
        p->code = code;
        p->cap = cap;
    }
    p->code[p->n] = in;
    return p->n++;
}

static int hole_add(Prog *p, int inst, int field, int next)
{
    if (inst < 0 || p->nh >= (int)(sizeof(p->holes) / sizeof(p->holes[0]))) {
        p->failed = 1;
        return -1;
    }
    int i = p->nh++;
    p->holes[i].inst = inst;
    p->holes[i].field = field;
    p->holes[i].next = next;
    return i;
}

static void patch(Prog *p, int list, int target)
{
    while (list >= 0) {
        int next = p->holes[list].next;
        struct ds4_re_inst *in = &p->code[p->holes[list].inst];
        if (p->holes[list].field) in->y = target;
        else in->x = target;
        list = next;
    }
}

static struct ds4_re_inst op1(int op, int x)
{
    struct ds4_re_inst in;
    memset(&in, 0, sizeof(in));
    in.op = op;
    in.x = x;
    in.y = -1;
    return in;
}

static void class_set(unsigned char *cls, unsigned char c, int icase)
{
    cls[c >> 3] |= (unsigned char)(1u << (c & 7));
    if (!icase) return;
    if (c >= 'A' && c <= 'Z') class_set(cls, (unsigned char)(c - 'A' + 'a'), 0);
    if (c >= 'a' && c <= 'z') class_set(cls, (unsigned char)(c - 'a' + 'A'), 0);
}

static int class_has(const unsigned char *cls, unsigned char c)
{
    return (cls[c >> 3] >> (c & 7)) & 1;
}

static const char *skip_class(const char *p, const char *end)
{
    if (p >= end || *p != '[') return p;
    p++;
    if (p < end && *p == '^') p++;
    if (p < end && *p == ']') p++;
    while (p < end && *p != ']') {
        if (*p == '\\' && p + 1 < end) p += 2;
        else p++;
    }
    if (p < end && *p == ']') p++;
    return p;
}

static const char *skip_atom(const char *p, const char *end)
{
    if (p >= end) return NULL;
    if (*p == '(') {
        int depth = 1;
        p++;
        while (p < end && depth) {
            if (*p == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (*p == '[') {
                const char *n = skip_class(p, end);
                if (n == p) return NULL;
                p = n;
                continue;
            }
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            if (depth) p++;
        }
        if (depth) return NULL;
        return p + 1;
    }
    if (*p == '[') {
        const char *n = skip_class(p, end);
        return n == p ? NULL : n;
    }
    if (*p == '\\') return p + 1 < end ? p + 2 : NULL;
    if (*p == '|' || *p == ')' || *p == '*' || *p == '+' || *p == '?' || *p == '{')
        return NULL;
    return p + 1;
}

static Frag compile_alt(Prog *p, const char **pp, const char *end);

static int compile_class(Prog *prog, const char **pp, const char *end)
{
    const char *s = *pp;
    struct ds4_re_inst in;
    memset(&in, 0, sizeof(in));
    in.op = OP_CLASS;
    in.y = -1;
    if (*s != '[') return -1;
    s++;
    if (s < end && *s == '^') {
        in.x = 1;
        s++;
    }
    int first = 1;
    while (s < end && (first || *s != ']')) {
        first = 0;
        if (*s == ']' && s != *pp + 1 && !(in.x && s == *pp + 2)) break;
        unsigned char a;
        if (*s == '\\' && s + 1 < end) {
            s++;
            a = (unsigned char)*s++;
        } else {
            a = (unsigned char)*s++;
        }
        if (s + 1 < end && *s == '-' && s[1] != ']') {
            s++;
            unsigned char b = (unsigned char)*s++;
            if (b < a) {
                prog->failed = 1;
                return -1;
            }
            for (int c = a; c <= b; c++)
                class_set(in.cls, (unsigned char)c, prog->icase);
        } else {
            class_set(in.cls, a, prog->icase);
        }
    }
    if (s >= end || *s != ']') {
        prog->failed = 1;
        return -1;
    }
    *pp = s + 1;
    return emit(prog, in);
}

static Frag compile_atom(Prog *p, const char **pp, const char *end)
{
    Frag bad = {-1, -1, 0};
    if (p->failed || *pp >= end) return bad;
    const char *s = *pp;
    if (*s == '(') {
        (*pp)++;
        Frag inner = compile_alt(p, pp, end);
        if (inner.start < 0 || *pp >= end || **pp != ')') {
            p->failed = 1;
            return bad;
        }
        (*pp)++;
        return inner;
    }
    if (*s == '[') {
        int at = compile_class(p, pp, end);
        if (at < 0) return bad;
        Frag f = {at, -1, 1};
        return f;
    }
    if (*s == '.') {
        (*pp)++;
        Frag f = {emit(p, op1(OP_ANY, 0)), -1, 1};
        return f;
    }
    if (*s == '^') {
        (*pp)++;
        Frag f = {emit(p, op1(OP_BOL, 0)), -1, 1};
        return f;
    }
    if (*s == '$') {
        (*pp)++;
        Frag f = {emit(p, op1(OP_EOL, 0)), -1, 1};
        return f;
    }
    unsigned char ch;
    if (*s == '\\') {
        if (s + 1 >= end) {
            p->failed = 1;
            return bad;
        }
        ch = (unsigned char)s[1];
        *pp = s + 2;
    } else {
        ch = (unsigned char)*s;
        *pp = s + 1;
    }
    Frag f = {emit(p, op1(OP_CHAR, ch)), -1, 1};
    return f;
}

static int parse_bound(const char **pp, const char *end, int *n)
{
    if (*pp >= end || !isdigit((unsigned char)**pp)) return -1;
    int v = 0;
    while (*pp < end && isdigit((unsigned char)**pp)) {
        v = v * 10 + (**pp - '0');
        if (v > 128) return -1;
        (*pp)++;
    }
    *n = v;
    return 0;
}

static Frag join_seq(Prog *p, Frag a, Frag b)
{
    if (a.start < 0) return a;
    if (b.start < 0) return b;
    patch(p, a.out, b.start);
    Frag r;
    r.start = a.start;
    r.out = b.out;
    r.fall = b.fall;
    return r;
}

static Frag compile_repeat(Prog *p, const char **pp, const char *end)
{
    Frag bad = {-1, -1, 0};
    Frag empty = {p->n, -1, 1};
    if (*pp >= end || **pp == ')' || **pp == '|') return empty;
    const char *atom_end = skip_atom(*pp, end);
    if (!atom_end) {
        p->failed = 1;
        return bad;
    }
    char q = atom_end < end ? *atom_end : 0;
    if (q == '*' || q == '+' || q == '?' || q == '{') {
        const char *atom_s = *pp;
        if (q == '{') {
            const char *bp = atom_end + 1;
            int lo = 0, hi = 0, inf = 0;
            if (parse_bound(&bp, end, &lo) != 0 || bp >= end) {
                p->failed = 1;
                return bad;
            }
            if (*bp == '}') {
                hi = lo;
                bp++;
            } else if (*bp == ',') {
                bp++;
                if (bp < end && *bp == '}') {
                    inf = 1;
                    bp++;
                } else if (parse_bound(&bp, end, &hi) != 0 || bp >= end || *bp != '}') {
                    p->failed = 1;
                    return bad;
                } else {
                    bp++;
                }
            } else {
                p->failed = 1;
                return bad;
            }
            if (!inf && hi < lo) {
                p->failed = 1;
                return bad;
            }
            *pp = atom_s;
            Frag acc = {p->n, -1, 1};
            int any = 0;
            for (int i = 0; i < lo; i++) {
                const char *one = atom_s;
                Frag part = compile_atom(p, &one, atom_end);
                acc = any ? join_seq(p, acc, part) : part;
                any = 1;
            }
            int opt = inf ? 1 : hi - lo;
            if (inf) {
                int split = emit(p, op1(OP_SPLIT, 0));
                const char *one = atom_s;
                Frag body = compile_atom(p, &one, atom_end);
                int jmp = emit(p, op1(OP_JMP, split));
                if (split < 0 || jmp < 0 || body.start < 0) return bad;
                p->code[split].x = body.start;
                p->code[split].y = -1;
                patch(p, body.out, jmp);
                Frag star = {split, hole_add(p, split, 1, -1), 0};
                acc = any ? join_seq(p, acc, star) : star;
            } else {
                for (int i = 0; i < opt; i++) {
                    int split = emit(p, op1(OP_SPLIT, 0));
                    const char *one = atom_s;
                    Frag body = compile_atom(p, &one, atom_end);
                    if (split < 0 || body.start < 0) return bad;
                    p->code[split].x = body.start;
                    p->code[split].y = -1;
                    Frag qf = {split, hole_add(p, split, 1, body.out), body.fall};
                    acc = any || lo ? join_seq(p, acc, qf) : qf;
                    any = 1;
                }
            }
            *pp = bp;
            if (bp < end && (*bp == '*' || *bp == '+' || *bp == '?' || *bp == '{')) {
                p->failed = 1;
                return bad;
            }
            return any ? acc : empty;
        }
        if (q == '?' || q == '*') {
            int split = emit(p, op1(OP_SPLIT, 0));
            Frag body = compile_atom(p, pp, atom_end);
            if (**pp != q) {
                p->failed = 1;
                return bad;
            }
            (*pp)++;
            if (*pp < end && (**pp == '*' || **pp == '+' || **pp == '?' || **pp == '{')) {
                p->failed = 1;
                return bad;
            }
            if (split < 0 || body.start < 0) return bad;
            p->code[split].x = body.start;
            p->code[split].y = -1;
            if (q == '*') {
                int jmp = emit(p, op1(OP_JMP, split));
                if (jmp < 0) return bad;
                patch(p, body.out, jmp);
                Frag r = {split, hole_add(p, split, 1, -1), 0};
                return r;
            }
            Frag r = {split, hole_add(p, split, 1, body.out), body.fall};
            return r;
        }
        Frag body = compile_atom(p, pp, atom_end);
        if (**pp != '+') {
            p->failed = 1;
            return bad;
        }
        (*pp)++;
        if (*pp < end && (**pp == '*' || **pp == '+' || **pp == '?' || **pp == '{')) {
            p->failed = 1;
            return bad;
        }
        int split = emit(p, op1(OP_SPLIT, body.start));
        if (split < 0 || body.start < 0) return bad;
        p->code[split].y = -1;
        patch(p, body.out, split);
        Frag r = {body.start, hole_add(p, split, 1, -1), 0};
        return r;
    }
    return compile_atom(p, pp, end);
}

static int chain_holes(Prog *p, int a, int b)
{
    if (a < 0) return b;
    if (b < 0) return a;
    int tail = a;
    while (p->holes[tail].next >= 0) tail = p->holes[tail].next;
    p->holes[tail].next = b;
    return a;
}

static int top_bar(const char *p, const char *end)
{
    int depth = 0;
    for (; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            continue;
        }
        if (*p == '[') {
            const char *n = skip_class(p, end);
            if (n == p) return 0;
            p = n - 1;
            continue;
        }
        if (*p == '(') depth++;
        else if (*p == ')') {
            if (depth == 0) return 0;
            depth--;
        } else if (*p == '|' && depth == 0) {
            return 1;
        }
    }
    return 0;
}

static Frag compile_concat(Prog *p, const char **pp, const char *end)
{
    Frag acc = {p->n, -1, 1};
    int any = 0;
    while (*pp < end && **pp != ')' && **pp != '|') {
        Frag part = compile_repeat(p, pp, end);
        if (part.start < 0) return part;
        if (!any) acc = part;
        else acc = join_seq(p, acc, part);
        any = 1;
        if (p->failed) {
            Frag bad = {-1, -1, 0};
            return bad;
        }
    }
    return acc;
}

static Frag compile_alt(Prog *p, const char **pp, const char *end)
{
    /* A bar's SPLIT is the entry, so it has to be the first instruction of
     * the fragment. Otherwise a preceding piece would fall into the left arm
     * and never try the right one. */
    if (!top_bar(*pp, end)) return compile_concat(p, pp, end);
    int sp = emit(p, op1(OP_SPLIT, -1));
    Frag left = compile_concat(p, pp, end);
    if (left.fall) {
        int jmp = emit(p, op1(OP_JMP, -1));
        left.out = hole_add(p, jmp, 0, left.out);
        left.fall = 0;
    }
    if (sp < 0 || left.start < 0 || *pp >= end || **pp != '|') {
        p->failed = 1;
        Frag bad = {-1, -1, 0};
        return bad;
    }
    (*pp)++;
    Frag right = compile_alt(p, pp, end);
    if (right.start < 0) {
        Frag bad = {-1, -1, 0};
        return bad;
    }
    p->code[sp].x = left.start;
    p->code[sp].y = right.start;
    Frag r;
    r.start = sp;
    r.out = chain_holes(p, left.out, right.out);
    r.fall = right.fall;
    return r;
}

static int fold_char(int c, int icase)
{
    if (icase && c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    if (icase && c >= 'a' && c <= 'z') return c;
    return c;
}

typedef struct {
    const struct ds4_re_inst *code;
    const char *origin;
    const char *end;
    int icase;
    int fuel;
    int ips[64];
    const char *sps[64];
    int sp;
} Exec;

static int exec_at(Exec *e, int ip, const char *s)
{
    while (ip >= 0 && e->fuel-- > 0) {
        const struct ds4_re_inst *in = &e->code[ip];
        switch (in->op) {
        case OP_CHAR:
            if (s >= e->end) return 0;
            if (fold_char((unsigned char)*s, e->icase) != fold_char(in->x, e->icase))
                return 0;
            s++;
            ip++;
            break;
        case OP_ANY:
            if (s >= e->end || *s == '\n') return 0;
            s++;
            ip++;
            break;
        case OP_CLASS: {
            if (s >= e->end) return 0;
            int has = class_has(in->cls, (unsigned char)*s);
            if (e->icase) {
                unsigned char c = (unsigned char)*s;
                if (c >= 'A' && c <= 'Z') has |= class_has(in->cls, (unsigned char)(c - 'A' + 'a'));
                if (c >= 'a' && c <= 'z') has |= class_has(in->cls, (unsigned char)(c - 'a' + 'A'));
            }
            if (in->x) has = !has;
            if (!has) return 0;
            s++;
            ip++;
            break;
        }
        case OP_BOL:
            if (s != e->origin) return 0;
            ip++;
            break;
        case OP_EOL:
            if (s != e->end) return 0;
            ip++;
            break;
        case OP_JMP:
            ip = in->x;
            break;
        case OP_SPLIT: {
            int loop = 0;
            for (int i = 0; i < e->sp; i++) {
                if (e->ips[i] == ip && e->sps[i] == s) loop = 1;
            }
            int saved = e->sp;
            if (e->sp < 64) {
                e->ips[e->sp] = ip;
                e->sps[e->sp] = s;
                e->sp++;
            }
            if (!loop && exec_at(e, in->x, s)) return 1;
            e->sp = saved;
            ip = in->y;
            break;
        }
        case OP_MATCH:
            return 1;
        default:
            return 0;
        }
    }
    return 0;
}

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
    if (!preg) return REG_BADPAT;
    memset(preg, 0, sizeof(*preg));
    if (!pattern) return REG_BADPAT;
    Prog p;
    memset(&p, 0, sizeof(p));
    p.icase = (cflags & REG_ICASE) != 0;
    const char *cursor = pattern;
    const char *end = pattern + strlen(pattern);
    Frag f = compile_alt(&p, &cursor, end);
    if (p.failed || f.start < 0 || cursor != end) {
        free(p.code);
        return p.failed && !p.code ? REG_ESPACE : REG_BADPAT;
    }
    int match = emit(&p, op1(OP_MATCH, 0));
    if (match < 0) {
        free(p.code);
        return REG_ESPACE;
    }
    patch(&p, f.out, match);
    preg->code = (ds4_re_inst *)p.code;
    preg->n = p.n;
    preg->start = f.start;
    preg->icase = p.icase;
    return 0;
}

int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)nmatch;
    (void)pmatch;
    (void)eflags;
    if (!preg || !preg->code || !string) return REG_BADPAT;
    const char *end = string + strlen(string);
    for (const char *s = string;; s++) {
        Exec ex;
        memset(&ex, 0, sizeof(ex));
        ex.code = (const struct ds4_re_inst *)preg->code;
        ex.origin = string;
        ex.end = end;
        ex.icase = preg->icase;
        ex.fuel = 2000000;
        if (exec_at(&ex, preg->start, s)) return 0;
        if (s == end) break;
    }
    return REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
    (void)preg;
    const char *msg = "regex error";
    if (errcode == REG_NOMATCH) msg = "no match";
    else if (errcode == REG_BADPAT) msg = "bad pattern";
    else if (errcode == REG_ESPACE) msg = "out of memory";
    size_t n = strlen(msg);
    if (errbuf && errbuf_size) {
        size_t c = n < errbuf_size - 1 ? n : errbuf_size - 1;
        memcpy(errbuf, msg, c);
        errbuf[c] = '\0';
    }
    return n + 1;
}

void regfree(regex_t *preg)
{
    if (!preg) return;
    free(preg->code);
    preg->code = NULL;
    preg->n = 0;
}

#ifdef DS4_REGEX_TEST
#include <stdio.h>
static int expect(const char *pat, int flags, const char *text, int want)
{
    regex_t re;
    int rc = regcomp(&re, pat, flags);
    if (rc != 0) {
        fprintf(stderr, "compile failed %s\n", pat);
        return 1;
    }
    int got = regexec(&re, text, 0, NULL, 0) == 0;
    regfree(&re);
    if (got != want) {
        fprintf(stderr, "pat '%s' text '%s' got %d want %d\n", pat, text, got, want);
        return 1;
    }
    return 0;
}
int main(void)
{
    int n = 0;
    n += expect("abc", 0, "zzabc", 1);
    n += expect("abc", 0, "ab", 0);
    n += expect("a|b", 0, "b", 1);
    n += expect("a*", 0, "", 1);
    n += expect("a*", 0, "aaa", 1);
    n += expect("^a", 0, "ba", 0);
    n += expect("^a", 0, "ab", 1);
    n += expect("a$", 0, "ba", 1);
    n += expect("a$", 0, "ab", 0);
    n += expect("[0-9]+", 0, "x42y", 1);
    n += expect("(ab)+", 0, "abab", 1);
    n += expect("(ab)+", 0, "a", 0);
    n += expect("a?b", 0, "b", 1);
    n += expect("a?b", 0, "ab", 1);
    n += expect("Foo", REG_ICASE, "foo", 1);
    n += expect("a.b", 0, "a\nb", 0);
    n += expect("a.b", 0, "axb", 1);
    n += expect("a|", 0, "zzz", 1);
    n += expect("|b", 0, "b", 1);
    n += expect("a{2,3}", 0, "aa", 1);
    n += expect("a{2,3}", 0, "a", 0);
    return n ? 1 : 0;
}
#endif
