/*
 * Banking Management System - concurrent server.
 *
 * One process per client (fork). A System V counting semaphore caps the number
 * of simultaneous clients; each child takes a slot with SEM_UNDO so the slot is
 * returned automatically however the child ends.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bank.h"

union semun { int val; struct semid_ds *buf; unsigned short *array; };

static int semid = -1;
static int listen_fd = -1;

static void say(const char *s) { if (write(STDOUT_FILENO, s, strlen(s)) < 0) return; }

static void shutdown_server(int sig)
{
    (void)sig;
    if (semid >= 0) semctl(semid, 0, IPC_RMID);
    if (listen_fd >= 0) close(listen_fd);
    say("\nServer stopped.\n");
    _exit(0);
}

static void init_storage(void)
{
    if (mkdir(DATA_DIR, 0700) < 0 && errno != EEXIST) { perror("mkdir " DATA_DIR); exit(1); }

    int fd = db_open(ADMIN_FILE);
    if (fd < 0) { perror("open " ADMIN_FILE); exit(1); }
    int count = rec_count(fd, sizeof(Admin));
    close(fd);
    if (count == 0) {
        Admin a = {0};
        strcpy(a.first_name, "System");
        strcpy(a.last_name, "Admin");
        a.password = hash_password(DEFAULT_ADMIN_PASSWORD);
        int id = append_rec(ADMIN_FILE, &a, sizeof a);
        char msg[128];
        snprintf(msg, sizeof msg, "Created default administrator: ID %d, password '%s'\n", id, DEFAULT_ADMIN_PASSWORD);
        say(msg);
    }
}

static int do_login(int cfd, int role)
{
    int id;
    char pw[PASS_LEN], name[2 * NAME_LEN + 2];
    sendf(cfd, "\n--- %s Login ---\n", role_name(role));
    if (!ask_int(cfd, "User ID: ", &id)) return MENU_LOGOUT;
    ask_secret(cfd, "Password: ", pw, sizeof pw);

    if (!validate_login(role, id, pw, name, sizeof name)) {
        send_str(cfd, "Login failed: invalid credentials or inactive account.\n");
        return MENU_LOGOUT;
    }
    int session = session_acquire(role, id);
    if (session < 0) {
        send_str(cfd, "Login failed: this user is already logged in from another session.\n");
        return MENU_LOGOUT;
    }

    sendf(cfd, "\nWelcome, %s! (%s, ID %d)\n", name, role_name(role), id);
    int result;
    switch (role) {
    case ROLE_CUSTOMER: result = customer_menu(cfd, id); break;
    case ROLE_EMPLOYEE: result = employee_menu(cfd, id); break;
    case ROLE_MANAGER:  result = manager_menu(cfd, id); break;
    default:            result = admin_menu(cfd, id); break;
    }
    session_release(session);
    if (result == MENU_LOGOUT) send_str(cfd, "Logged out.\n");
    return result;
}

static void handle_client(int cfd)
{
    struct sembuf take = {0, -1, IPC_NOWAIT | SEM_UNDO};
    if (semop(semid, &take, 1) < 0) {
        send_str(cfd, "Server is busy, please try again later.\n");
        return;
    }

    static const char *const items[] = {"Customer Login", "Employee Login", "Manager Login", "Administrator Login", "Exit"};
    static const int roles[] = {ROLE_CUSTOMER, ROLE_EMPLOYEE, ROLE_MANAGER, ROLE_ADMIN};
    send_str(cfd, "\n*** Welcome to the Banking Management System ***\n");
    for (;;) {
        int choice = menu_choice(cfd, "Main Menu", items, 5);
        if (choice == 5) break;
        if (choice && do_login(cfd, roles[choice - 1]) == MENU_EXIT) break;
    }
    send_str(cfd, "Thank you for banking with us. Goodbye!\n");
}

int main(int argc, char *argv[])
{
    int port = argc > 1 ? atoi(argv[1]) : SERVER_PORT;
    init_storage();

    semid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    if (semid < 0) { perror("semget"); return 1; }
    union semun arg = {.val = MAX_CLIENTS};
    if (semctl(semid, 0, SETVAL, arg) < 0) { perror("semctl"); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;           /* children are reaped automatically, no zombies */
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, NULL);        /* write() to a dead client returns EPIPE instead */
    sa.sa_handler = shutdown_server;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 32) < 0) { perror("listen"); return 1; }

    char msg[96];
    snprintf(msg, sizeof msg, "Bank server listening on port %d (pid %d)\n", port, getpid());
    say(msg);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t len = sizeof peer;
        int cfd = accept(listen_fd, (struct sockaddr *)&peer, &len);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            close(listen_fd);
            handle_client(cfd);
            close(cfd);
            _exit(0);
        }
        if (pid < 0) perror("fork");
        close(cfd);
    }
}
