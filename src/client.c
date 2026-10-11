/*
 * Banking Management System - client.
 * A thin terminal: prints whatever the server sends and, whenever the server
 * sends a prompt marker, reads one line from stdin and sends it back.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"

static void write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            exit(1);
        }
        p += w;
        n -= (size_t)w;
    }
}

/* Reads one line from stdin byte by byte, so piped input is not over-consumed. */
static int read_line(char *buf, size_t n)
{
    size_t i = 0;
    char c;
    for (;;) {
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return i > 0 ? (int)i : -1;
        if (c == '\n') break;
        if (i + 2 < n) buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

static int read_secret(char *buf, size_t n)
{
    struct termios old, quiet;
    int tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    int r = read_line(buf, n);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        write_all(STDOUT_FILENO, "\n", 1);
    }
    return r;
}

int main(int argc, char *argv[])
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : SERVER_PORT;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { fprintf(stderr, "Invalid address: %s\n", host); return 1; }
    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    char buf[4096], line[512];
    for (;;) {
        ssize_t n = read(sock, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;                       /* server closed the connection */

        ssize_t start = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != PROMPT_MARK && buf[i] != SECRET_MARK) continue;
            write_all(STDOUT_FILENO, buf + start, (size_t)(i - start));
            int len = buf[i] == SECRET_MARK ? read_secret(line, sizeof line) : read_line(line, sizeof line);
            if (len < 0) { close(sock); return 0; }   /* stdin closed */
            line[len] = '\n';
            write_all(sock, line, (size_t)len + 1);
            start = i + 1;
        }
        write_all(STDOUT_FILENO, buf + start, (size_t)(n - start));
    }
    close(sock);
    return 0;
}
