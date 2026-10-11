/* Customer role. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

static void view_balance(int cfd, int id) { show_customer_summary(cfd, id); }

/* Deposit (withdraw = 0) or withdraw (withdraw = 1) under an exclusive record lock. */
static void deposit_withdraw(int cfd, int id, int withdraw)
{
    double amount;
    if (!ask_amount(cfd, withdraw ? "Amount to withdraw: " : "Amount to deposit: ", &amount, 0)) return;

    char msg[160] = "Account unavailable.\n";
    int fd = db_open(CUSTOMER_FILE);
    Customer c;

    lock_rec(fd, id, sizeof c, F_WRLCK);                 /* ---- critical section ---- */
    if (read_rec(fd, id, &c, sizeof c) == 0 && c.active) {
        if (withdraw && c.balance < amount) {
            snprintf(msg, sizeof msg, "Insufficient balance. Available: Rs. %.2f\n", c.balance);
        } else {
            c.balance += withdraw ? -amount : amount;
            if (write_rec(fd, id, &c, sizeof c) == 0) {
                log_transaction(id, withdraw ? TXN_WITHDRAW : TXN_DEPOSIT, amount, c.balance, 0);
                snprintf(msg, sizeof msg, "%s of Rs. %.2f successful. New balance: Rs. %.2f\n",
                         withdraw ? "Withdrawal" : "Deposit", amount, c.balance);
            }
        }
    }
    unlock_rec(fd, id, sizeof c);                        /* -------------------------- */
    close(fd);
    send_str(cfd, msg);
}

/*
 * Both account records are locked before anything is read, always in
 * ascending account-number order, so two opposite transfers (A->B, B->A)
 * can never deadlock. Either both balances change or neither does.
 */
static void transfer_funds(int cfd, int id)
{
    int to;
    if (!ask_int(cfd, "Recipient account number: ", &to)) return;
    if (to == id) { send_str(cfd, "Cannot transfer to your own account.\n"); return; }
    if (to <= 0) { send_str(cfd, "Invalid account number.\n"); return; }
    double amount;
    if (!ask_amount(cfd, "Amount to transfer: ", &amount, 0)) return;

    char msg[160];
    int fd = db_open(CUSTOMER_FILE);
    int lo = id < to ? id : to, hi = id < to ? to : id;
    Customer src, dst;

    lock_rec(fd, lo, sizeof src, F_WRLCK);               /* ---- critical section ---- */
    lock_rec(fd, hi, sizeof src, F_WRLCK);
    if (read_rec(fd, id, &src, sizeof src) < 0 || !src.active) {
        snprintf(msg, sizeof msg, "Your account is unavailable.\n");
    } else if (read_rec(fd, to, &dst, sizeof dst) < 0 || !dst.active) {
        snprintf(msg, sizeof msg, "Recipient account %d does not exist or is inactive.\n", to);
    } else if (src.balance < amount) {
        snprintf(msg, sizeof msg, "Insufficient balance. Available: Rs. %.2f\n", src.balance);
    } else {
        Customer src_old = src;
        src.balance -= amount;
        dst.balance += amount;
        if (write_rec(fd, id, &src, sizeof src) < 0) {
            snprintf(msg, sizeof msg, "Transfer failed.\n");
        } else if (write_rec(fd, to, &dst, sizeof dst) < 0) {
            write_rec(fd, id, &src_old, sizeof src_old);  /* roll back the debit */
            snprintf(msg, sizeof msg, "Transfer failed.\n");
        } else {
            log_transaction(id, TXN_TRANSFER_OUT, amount, src.balance, to);
            log_transaction(to, TXN_TRANSFER_IN, amount, dst.balance, id);
            snprintf(msg, sizeof msg, "Transferred Rs. %.2f to account %d. New balance: Rs. %.2f\n", amount, to, src.balance);
        }
    }
    unlock_rec(fd, hi, sizeof src);
    unlock_rec(fd, lo, sizeof src);                      /* -------------------------- */
    close(fd);
    send_str(cfd, msg);
}

static void apply_for_loan(int cfd, int id)
{
    Loan l = {0};
    if (!ask_amount(cfd, "Loan amount: ", &l.amount, 0)) return;
    l.customer_id = id;
    l.status = LOAN_PENDING;
    l.applied_at = time(NULL);
    int lid = append_rec(LOAN_FILE, &l, sizeof l);
    if (lid < 0) send_str(cfd, "Could not submit loan application.\n");
    else sendf(cfd, "Loan application #%d for Rs. %.2f submitted. Status: PENDING\n", lid, l.amount);
}

static void view_my_loans(int cfd, int id)
{
    int fd = db_open(LOAN_FILE);
    StrBuf sb = {0};
    sb_printf(&sb, "\n%-6s %14s %-11s %s\n", "LoanID", "Amount", "Status", "Applied on");
    int found = 0;
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Loan));
    for (int i = 1; i <= n; i++) {
        Loan l;
        if (read_rec(fd, i, &l, sizeof l) < 0 || l.customer_id != id) continue;
        char when[32];
        format_time(l.applied_at, when, sizeof when);
        sb_printf(&sb, "%-6d %14.2f %-11s %s\n", l.id, l.amount, loan_status_name(l.status), when);
        found++;
    }
    unlock_all(fd);
    close(fd);
    if (!found) sb_printf(&sb, "(no loan applications)\n");
    sb_send(cfd, &sb);
}

static void add_feedback(int cfd, int id)
{
    Feedback f = {0};
    ask(cfd, "Your feedback: ", f.message, sizeof f.message);
    if (!f.message[0]) { send_str(cfd, "Feedback cannot be empty.\n"); return; }
    f.customer_id = id;
    f.created_at = time(NULL);
    if (append_rec(FEEDBACK_FILE, &f, sizeof f) < 0) send_str(cfd, "Could not save feedback.\n");
    else send_str(cfd, "Thank you! Your feedback has been recorded.\n");
}

int customer_menu(int cfd, int id)
{
    static const char *const items[] = {
        "View Account Balance", "Deposit Money", "Withdraw Money", "Transfer Funds",
        "Apply for a Loan", "View My Loan Applications", "Change Password", "Add Feedback",
        "View Transaction History", "Logout", "Exit",
    };
    for (;;) {
        switch (menu_choice(cfd, "Customer Menu", items, 11)) {
        case 1:  view_balance(cfd, id); break;
        case 2:  deposit_withdraw(cfd, id, 0); break;
        case 3:  deposit_withdraw(cfd, id, 1); break;
        case 4:  transfer_funds(cfd, id); break;
        case 5:  apply_for_loan(cfd, id); break;
        case 6:  view_my_loans(cfd, id); break;
        case 7:  change_password(cfd, CUSTOMER_FILE, id, sizeof(Customer), offsetof(Customer, password)); break;
        case 8:  add_feedback(cfd, id); break;
        case 9:  view_transaction_history(cfd, id); break;
        case 10: return MENU_LOGOUT;
        case 11: return MENU_EXIT;
        }
    }
}
