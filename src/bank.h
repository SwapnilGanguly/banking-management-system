/* Server-side data model and module interfaces. */
#ifndef BANK_H
#define BANK_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "common.h"

/* ---------- storage ---------- */
#define DATA_DIR       "data"
#define CUSTOMER_FILE  DATA_DIR "/customers.dat"
#define EMPLOYEE_FILE  DATA_DIR "/employees.dat"   /* employees + managers */
#define ADMIN_FILE     DATA_DIR "/admins.dat"
#define LOAN_FILE      DATA_DIR "/loans.dat"
#define FEEDBACK_FILE  DATA_DIR "/feedback.dat"
#define TXN_FILE       DATA_DIR "/transactions.dat"
#define SESSION_CUST   DATA_DIR "/sessions_customer.lck"
#define SESSION_EMP    DATA_DIR "/sessions_employee.lck"
#define SESSION_ADMIN  DATA_DIR "/sessions_admin.lck"

#define MAX_CLIENTS    64
#define NAME_LEN       32
#define PASS_LEN       64
#define MSG_LEN        256
#define DEFAULT_ADMIN_PASSWORD "admin123"

/*
 * Every record type starts with `int id`. Records are fixed size and stored
 * at offset (id - 1) * sizeof(record), so a single record can be locked with
 * an fcntl() byte-range lock without blocking the rest of the file.
 */
typedef struct {
    int id;
    char first_name[NAME_LEN];
    char last_name[NAME_LEN];
    unsigned long long password;   /* hash */
    double balance;
    double loan;                   /* total sanctioned loan amount */
    int active;
} Customer;

enum { ROLE_CUSTOMER = 1, ROLE_EMPLOYEE, ROLE_MANAGER, ROLE_ADMIN };

typedef struct {
    int id;
    char first_name[NAME_LEN];
    char last_name[NAME_LEN];
    unsigned long long password;
    int role;                      /* ROLE_EMPLOYEE or ROLE_MANAGER */
    int active;
} Employee;

typedef struct {
    int id;
    char first_name[NAME_LEN];
    char last_name[NAME_LEN];
    unsigned long long password;
} Admin;

enum { LOAN_PENDING, LOAN_ASSIGNED, LOAN_PROCESSING, LOAN_APPROVED, LOAN_REJECTED };

typedef struct {
    int id;
    int customer_id;
    double amount;
    int status;
    int employee_id;               /* 0 until a manager assigns it */
    time_t applied_at;
} Loan;

typedef struct {
    int id;
    int customer_id;
    char message[MSG_LEN];
    int reviewed;
    time_t created_at;
} Feedback;

enum { TXN_DEPOSIT, TXN_WITHDRAW, TXN_TRANSFER_OUT, TXN_TRANSFER_IN, TXN_LOAN_CREDIT, TXN_ACCOUNT_OPEN };

typedef struct {
    int id;
    int customer_id;
    int type;
    double amount;
    double balance_after;
    int other_party;               /* counterpart account for transfers */
    time_t timestamp;
} Transaction;

/* Return values of the role menus. */
enum { MENU_LOGOUT, MENU_EXIT };

/* ---------- net.c : socket I/O helpers ---------- */
void send_str(int cfd, const char *s);
void sendf(int cfd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ask(int cfd, const char *prompt, char *buf, size_t n);
void ask_secret(int cfd, const char *prompt, char *buf, size_t n);
int  ask_int(int cfd, const char *prompt, int *out);
int  ask_amount(int cfd, const char *prompt, double *out, int allow_zero);
int  ask_name(int cfd, const char *prompt, char *buf, int allow_empty);
int  ask_new_password(int cfd, const char *prompt, unsigned long long *hash);
int  menu_choice(int cfd, const char *title, const char *const items[], int count);

typedef struct { char *data; size_t len, cap; } StrBuf;
void sb_printf(StrBuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_send(int cfd, StrBuf *sb);   /* sends and frees */

/* ---------- db.c : file records, locking, sessions ---------- */
int  db_open(const char *path);
int  lock_rec(int fd, int id, size_t size, short type);
int  unlock_rec(int fd, int id, size_t size);
int  lock_all(int fd, short type);
int  unlock_all(int fd);
int  read_rec(int fd, int id, void *rec, size_t size);
int  write_rec(int fd, int id, const void *rec, size_t size);
int  rec_count(int fd, size_t size);
int  append_rec(const char *path, void *rec, size_t size);
int  load_rec(const char *path, int id, void *rec, size_t size);
int  session_acquire(int role, int id);
void session_release(int fd);
unsigned long long hash_password(const char *pw);
void log_transaction(int customer_id, int type, double amount, double balance_after, int other);

/* ---------- ops.c : operations shared by several roles ---------- */
const char *loan_status_name(int status);
const char *role_name(int role);
void format_time(time_t t, char *buf, size_t n);
void show_customer_summary(int cfd, int customer_id);
void view_transaction_history(int cfd, int customer_id);
void modify_customer_details(int cfd);
void change_password(int cfd, const char *path, int id, size_t size, size_t pw_offset);
int  validate_login(int role, int id, const char *password, char *name, size_t n);

/* ---------- role menus ---------- */
int customer_menu(int cfd, int id);
int employee_menu(int cfd, int id);
int manager_menu(int cfd, int id);
int admin_menu(int cfd, int id);

#endif
