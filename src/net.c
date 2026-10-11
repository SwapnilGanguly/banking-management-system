/* Socket I/O helpers used by the per-client server process. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

/*
 * A write/read failure means the client went away. The handler process simply
 * exits: the kernel then drops every fcntl() lock it held (including its
 * login session lock) and SEM_UNDO gives back its client slot.
 * No socket I/O is ever done in the middle of a multi-record update, so an
 * exit here can never leave a half-applied transaction behind.
 */
static void client_gone(void) { _exit(0); }

static void send_all(int cfd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(cfd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            client_gone();
        }
        p += w;
        n -= (size_t)w;
    }
}

void send_str(int cfd, const char *s) { send_all(cfd, s, strlen(s)); }

void sendf(int cfd, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) send_all(cfd, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void trim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    size_t start = 0;
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start) memmove(s, s + start, len - start + 1);
}

static void recv_line(int cfd, char *buf, size_t n)
{
    size_t i = 0;
    char c;
    for (;;) {
        ssize_t r = read(cfd, &c, 1);
        if (r == 0) client_gone();
        if (r < 0) {
            if (errno == EINTR) continue;
            client_gone();
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (i + 1 < n) buf[i++] = c;
    }
    buf[i] = '\0';
    trim(buf);
}

static void ask_marked(int cfd, const char *prompt, char *buf, size_t n, char mark)
{
    send_str(cfd, prompt);
    send_all(cfd, &mark, 1);
    recv_line(cfd, buf, n);
}

void ask(int cfd, const char *prompt, char *buf, size_t n) { ask_marked(cfd, prompt, buf, n, PROMPT_MARK); }

void ask_secret(int cfd, const char *prompt, char *buf, size_t n) { ask_marked(cfd, prompt, buf, n, SECRET_MARK); }

int ask_int(int cfd, const char *prompt, int *out)
{
    char buf[64], *end;
    ask(cfd, prompt, buf, sizeof buf);
    errno = 0;
    long v = strtol(buf, &end, 10);
    if (buf[0] == '\0' || *end != '\0' || errno || v < -1000000000L || v > 1000000000L) {
        send_str(cfd, "Invalid number.\n");
        return 0;
    }
    *out = (int)v;
    return 1;
}

int ask_amount(int cfd, const char *prompt, double *out, int allow_zero)
{
    char buf[64], *end;
    ask(cfd, prompt, buf, sizeof buf);
    errno = 0;
    double v = strtod(buf, &end);
    if (buf[0] == '\0' || *end != '\0' || errno || !isfinite(v) || v < 0 || (!allow_zero && v == 0) || v > 1e12) {
        send_str(cfd, "Invalid amount.\n");
        return 0;
    }
    *out = round(v * 100.0) / 100.0;   /* keep to paise */
    return 1;
}

int ask_name(int cfd, const char *prompt, char *buf, int allow_empty)
{
    char tmp[128];
    ask(cfd, prompt, tmp, sizeof tmp);
    if (tmp[0] == '\0') {
        if (allow_empty) { buf[0] = '\0'; return 1; }
        send_str(cfd, "Name cannot be empty.\n");
        return 0;
    }
    if (strlen(tmp) >= NAME_LEN) {
        sendf(cfd, "Name must be shorter than %d characters.\n", NAME_LEN);
        return 0;
    }
    strcpy(buf, tmp);
    return 1;
}

int ask_new_password(int cfd, const char *prompt, unsigned long long *hash)
{
    char p1[PASS_LEN], p2[PASS_LEN];
    ask_secret(cfd, prompt, p1, sizeof p1);
    if (strlen(p1) < 4) {
        send_str(cfd, "Password must have at least 4 characters.\n");
        return 0;
    }
    ask_secret(cfd, "Confirm password: ", p2, sizeof p2);
    if (strcmp(p1, p2) != 0) {
        send_str(cfd, "Passwords do not match.\n");
        return 0;
    }
    *hash = hash_password(p1);
    return 1;
}

int menu_choice(int cfd, const char *title, const char *const items[], int count)
{
    StrBuf sb = {0};
    sb_printf(&sb, "\n===== %s =====\n", title);
    for (int i = 0; i < count; i++) sb_printf(&sb, "%2d. %s\n", i + 1, items[i]);
    sb_send(cfd, &sb);
    int choice;
    if (!ask_int(cfd, "Enter your choice: ", &choice)) return 0;
    if (choice < 1 || choice > count) {
        send_str(cfd, "Invalid choice.\n");
        return 0;
    }
    return choice;
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        size_t room = sb->cap - sb->len;
        int n = vsnprintf(sb->data ? sb->data + sb->len : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < room) { sb->len += (size_t)n; return; }
        size_t cap = sb->cap ? sb->cap * 2 : 1024;
        while (cap - sb->len <= (size_t)n) cap *= 2;
        char *p = realloc(sb->data, cap);
        if (!p) return;
        sb->data = p;
        sb->cap = cap;
    }
}

void sb_send(int cfd, StrBuf *sb)
{
    if (sb->len) send_all(cfd, sb->data, sb->len);
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}
