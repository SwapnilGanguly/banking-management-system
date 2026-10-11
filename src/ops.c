/* Operations shared by more than one role. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

const char *loan_status_name(int status)
{
    switch (status) {
    case LOAN_PENDING:    return "PENDING";
    case LOAN_ASSIGNED:   return "ASSIGNED";
    case LOAN_PROCESSING: return "PROCESSING";
    case LOAN_APPROVED:   return "APPROVED";
    case LOAN_REJECTED:   return "REJECTED";
    }
    return "?";
}

const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "Customer";
    case ROLE_EMPLOYEE: return "Employee";
    case ROLE_MANAGER:  return "Manager";
    case ROLE_ADMIN:    return "Administrator";
    }
    return "?";
}

static const char *txn_type_name(int type)
{
    switch (type) {
    case TXN_DEPOSIT:      return "DEPOSIT";
    case TXN_WITHDRAW:     return "WITHDRAW";
    case TXN_TRANSFER_OUT: return "TRANSFER OUT";
    case TXN_TRANSFER_IN:  return "TRANSFER IN";
    case TXN_LOAN_CREDIT:  return "LOAN CREDIT";
    case TXN_ACCOUNT_OPEN: return "ACCOUNT OPEN";
    }
    return "?";
}

void format_time(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

void show_customer_summary(int cfd, int customer_id)
{
    Customer c;
    if (load_rec(CUSTOMER_FILE, customer_id, &c, sizeof c) < 0) {
        send_str(cfd, "Customer not found.\n");
        return;
    }
    sendf(cfd, "\nAccount No : %d\nName       : %s %s\nStatus     : %s\nBalance    : Rs. %.2f\nLoan       : Rs. %.2f\n",
          c.id, c.first_name, c.last_name, c.active ? "ACTIVE" : "DEACTIVATED", c.balance, c.loan);
}

/* Passbook view. The listing is built under a shared lock and sent after unlocking. */
void view_transaction_history(int cfd, int customer_id)
{
    int fd = db_open(TXN_FILE);
    if (fd < 0) { send_str(cfd, "Unable to open transaction log.\n"); return; }

    StrBuf sb = {0};
    sb_printf(&sb, "\n--- Transaction history of account %d ---\n", customer_id);
    sb_printf(&sb, "%-6s %-19s %-13s %12s %14s  %s\n", "TxnID", "Date/Time", "Type", "Amount", "Balance", "Details");
    int found = 0;

    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Transaction));
    for (int i = 1; i <= n; i++) {
        Transaction t;
        if (read_rec(fd, i, &t, sizeof t) < 0 || t.customer_id != customer_id) continue;
        char when[32], details[48] = "";
        format_time(t.timestamp, when, sizeof when);
        if (t.type == TXN_TRANSFER_OUT) snprintf(details, sizeof details, "to A/C %d", t.other_party);
        if (t.type == TXN_TRANSFER_IN)  snprintf(details, sizeof details, "from A/C %d", t.other_party);
        if (t.type == TXN_LOAN_CREDIT)  snprintf(details, sizeof details, "loan #%d", t.other_party);
        sb_printf(&sb, "%-6d %-19s %-13s %12.2f %14.2f  %s\n", t.id, when, txn_type_name(t.type), t.amount, t.balance_after, details);
        found++;
    }
    unlock_all(fd);
    close(fd);

    if (!found) sb_printf(&sb, "(no transactions)\n");
    sb_send(cfd, &sb);
}

/* Used by employees and the administrator. Blank input keeps the old value. */
void modify_customer_details(int cfd)
{
    int id;
    if (!ask_int(cfd, "Enter customer account number: ", &id)) return;
    show_customer_summary(cfd, id);

    Customer probe;
    if (load_rec(CUSTOMER_FILE, id, &probe, sizeof probe) < 0) return;

    char first[NAME_LEN], last[NAME_LEN];
    if (!ask_name(cfd, "New first name (Enter to keep): ", first, 1)) return;
    if (!ask_name(cfd, "New last name (Enter to keep): ", last, 1)) return;
    char yn[8];
    ask(cfd, "Reset password? (y/n): ", yn, sizeof yn);
    unsigned long long pw = 0;
    int reset = (yn[0] == 'y' || yn[0] == 'Y');
    if (reset && !ask_new_password(cfd, "New password: ", &pw)) return;

    int fd = db_open(CUSTOMER_FILE);
    Customer c;
    int ok = 0;
    lock_rec(fd, id, sizeof c, F_WRLCK);
    if (read_rec(fd, id, &c, sizeof c) == 0) {
        if (first[0]) strcpy(c.first_name, first);
        if (last[0]) strcpy(c.last_name, last);
        if (reset) c.password = pw;
        ok = write_rec(fd, id, &c, sizeof c) == 0;
    }
    unlock_rec(fd, id, sizeof c);
    close(fd);
    send_str(cfd, ok ? "Customer details updated.\n" : "Update failed.\n");
}

/* Generic for every record type: `pw_offset` is offsetof(<type>, password). */
void change_password(int cfd, const char *path, int id, size_t size, size_t pw_offset)
{
    char old[PASS_LEN];
    ask_secret(cfd, "Current password: ", old, sizeof old);
    unsigned long long old_hash = hash_password(old), new_hash;
    if (!ask_new_password(cfd, "New password: ", &new_hash)) return;

    union { Customer c; Employee e; Admin a; } rec;
    char *raw = (char *)&rec;
    const char *result = "Password change failed.\n";

    int fd = db_open(path);
    lock_rec(fd, id, size, F_WRLCK);
    if (read_rec(fd, id, raw, size) == 0) {
        unsigned long long cur;
        memcpy(&cur, raw + pw_offset, sizeof cur);
        if (cur != old_hash) {
            result = "Current password is incorrect.\n";
        } else {
            memcpy(raw + pw_offset, &new_hash, sizeof new_hash);
            if (write_rec(fd, id, raw, size) == 0) result = "Password changed successfully.\n";
        }
    }
    unlock_rec(fd, id, size);
    close(fd);
    send_str(cfd, result);
}

int validate_login(int role, int id, const char *password, char *name, size_t n)
{
    unsigned long long h = hash_password(password);
    if (role == ROLE_CUSTOMER) {
        Customer c;
        if (load_rec(CUSTOMER_FILE, id, &c, sizeof c) < 0 || c.password != h || !c.active) return 0;
        snprintf(name, n, "%s %s", c.first_name, c.last_name);
        return 1;
    }
    if (role == ROLE_EMPLOYEE || role == ROLE_MANAGER) {
        Employee e;
        if (load_rec(EMPLOYEE_FILE, id, &e, sizeof e) < 0 || e.password != h || !e.active || e.role != role) return 0;
        snprintf(name, n, "%s %s", e.first_name, e.last_name);
        return 1;
    }
    if (role == ROLE_ADMIN) {
        Admin a;
        if (load_rec(ADMIN_FILE, id, &a, sizeof a) < 0 || a.password != h) return 0;
        snprintf(name, n, "%s %s", a.first_name, a.last_name);
        return 1;
    }
    return 0;
}
