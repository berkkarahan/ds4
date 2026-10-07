/* POSIX regex surface for the Windows agent (REG_EXTENDED / REG_ICASE / REG_NOSUB).
 * MinGW and the MSVC ABI both lack <regex.h>. Only ds4-agent links the
 * implementation. */
#ifndef DS4_REGEX_WIN_H
#define DS4_REGEX_WIN_H

#include <stddef.h>

typedef struct ds4_re_inst ds4_re_inst;

typedef struct {
    ds4_re_inst *code;
    int n;
    int start;
    int icase;
} regex_t;

typedef struct {
    int rm_so;
    int rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    8
#define REG_NOTBOL   1
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ESPACE   3

int regcomp(regex_t *preg, const char *pattern, int cflags);
int regexec(const regex_t *preg, const char *string,
            size_t nmatch, regmatch_t pmatch[], int eflags);
size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size);
void regfree(regex_t *preg);

#endif
