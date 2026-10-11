/* Bank employee role. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

static void add_customer(int cfd)
{
    Customer c = {0};
    if (!ask_name(cfd, "First name: ", c.first_name, 0)) return;
    if (!ask_name(cfd, "Last name: ", c.last_name, 0)) return;
    if (!ask_new_password(cfd, "Initial password: ", &c.password)) return;
    if (!ask_amount(cfd, "Opening deposit (0 for none): ", &c.balance, 1)) return;
    c.active = 1;

    int id = append_rec(CUSTOMER_FILE, &c, sizeof c);
    if (id < 0) { send_str(cfd, "Could not create account.\n"); return; }
    log_transaction(id, TXN_ACCOUNT_OPEN, c.balance, c.balance, 0);
    sendf(cfd, "Account created. Account number (login ID): %d\n", id);
}

static void view_assigned_loans(int cfd, int emp_id)
{
    int fd = db_open(LOAN_FILE);
    StrBuf sb = {0};
    sb_printf(&sb, "\n--- Loan applications assigned to you ---\n%-6s %-8s %14s %-11s %s\n",
              "LoanID", "Cust", "Amount", "Status", "Applied on");
    int found = 0;
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Loan));
    for (int i = 1; i <= n; i++) {
        Loan l;
        if (read_rec(fd, i, &l, sizeof l) < 0 || l.employee_id != emp_id) continue;
        char when[32];
        format_time(l.applied_at, when, sizeof when);
        sb_printf(&sb, "%-6d %-8d %14.2f %-11s %s\n", l.id, l.customer_id, l.amount, loan_status_name(l.status), when);
        found++;
    }
    unlock_all(fd);
    close(fd);
    if (!found) sb_printf(&sb, "(none)\n");
    sb_send(cfd, &sb);
}

static int is_open_for(const Loan *l, int emp_id)
{
    return l->employee_id == emp_id && (l->status == LOAN_ASSIGNED || l->status == LOAN_PROCESSING);
}

/* Pick up an assigned application: mark it PROCESSING and show the applicant's profile. */
static void process_loan(int cfd, int emp_id)
{
    int lid;
    if (!ask_int(cfd, "Loan application ID: ", &lid)) return;

    int fd = db_open(LOAN_FILE);
    Loan l;
    int ok = 0;
    lock_rec(fd, lid, sizeof l, F_WRLCK);
    if (read_rec(fd, lid, &l, sizeof l) == 0 && is_open_for(&l, emp_id)) {
        l.status = LOAN_PROCESSING;
        ok = write_rec(fd, lid, &l, sizeof l) == 0;
    }
    unlock_rec(fd, lid, sizeof l);
    close(fd);

    if (!ok) { send_str(cfd, "No open loan application with that ID is assigned to you.\n"); return; }
    sendf(cfd, "\nLoan #%d is now PROCESSING. Requested amount: Rs. %.2f\nApplicant details:", l.id, l.amount);
    show_customer_summary(cfd, l.customer_id);
    send_str(cfd, "Use 'Approve/Reject Loans' to record your decision.\n");
}

/*
 * Approval credits the customer. Lock order is always loan record -> customer
 * record (customers never lock loans), so this cannot deadlock with them.
 */
static void approve_reject_loan(int cfd, int emp_id)
{
    int lid, decision;
    if (!ask_int(cfd, "Loan application ID: ", &lid)) return;
    if (!ask_int(cfd, "1. Approve  2. Reject : ", &decision)) return;
    if (decision != 1 && decision != 2) { send_str(cfd, "Invalid choice.\n"); return; }

    char msg[160] = "No open loan application with that ID is assigned to you.\n";
    int lfd = db_open(LOAN_FILE);
    Loan l;
    lock_rec(lfd, lid, sizeof l, F_WRLCK);               /* ---- critical section ---- */
    if (read_rec(lfd, lid, &l, sizeof l) == 0 && is_open_for(&l, emp_id)) {
        if (decision == 2) {
            l.status = LOAN_REJECTED;
            if (write_rec(lfd, lid, &l, sizeof l) == 0) snprintf(msg, sizeof msg, "Loan #%d rejected.\n", lid);
        } else {
            int cfd_db = db_open(CUSTOMER_FILE);
            Customer c;
            lock_rec(cfd_db, l.customer_id, sizeof c, F_WRLCK);
            if (read_rec(cfd_db, l.customer_id, &c, sizeof c) < 0 || !c.active) {
                snprintf(msg, sizeof msg, "Customer account is inactive; cannot disburse loan.\n");
            } else {
                Customer old = c;
                c.balance += l.amount;
                c.loan += l.amount;
                l.status = LOAN_APPROVED;
                if (write_rec(cfd_db, c.id, &c, sizeof c) < 0) {
                    snprintf(msg, sizeof msg, "Approval failed.\n");
                } else if (write_rec(lfd, lid, &l, sizeof l) < 0) {
                    write_rec(cfd_db, c.id, &old, sizeof old);   /* roll back */
                    snprintf(msg, sizeof msg, "Approval failed.\n");
                } else {
                    log_transaction(c.id, TXN_LOAN_CREDIT, l.amount, c.balance, lid);
                    snprintf(msg, sizeof msg, "Loan #%d approved. Rs. %.2f credited to account %d.\n", lid, l.amount, c.id);
                }
            }
            unlock_rec(cfd_db, l.customer_id, sizeof c);
            close(cfd_db);
        }
    }
    unlock_rec(lfd, lid, sizeof l);                      /* -------------------------- */
    close(lfd);
    send_str(cfd, msg);
}

static void view_passbook(int cfd)
{
    int id;
    if (!ask_int(cfd, "Customer account number: ", &id)) return;
    Customer c;
    if (load_rec(CUSTOMER_FILE, id, &c, sizeof c) < 0) { send_str(cfd, "Customer not found.\n"); return; }
    show_customer_summary(cfd, id);
    view_transaction_history(cfd, id);
}

int employee_menu(int cfd, int id)
{
    static const char *const items[] = {
        "Add New Customer", "Modify Customer Details", "View Assigned Loan Applications",
        "Process Loan Application", "Approve/Reject Loans", "View Customer Transactions (Passbook)",
        "Change Password", "Logout", "Exit",
    };
    for (;;) {
        switch (menu_choice(cfd, "Employee Menu", items, 9)) {
        case 1: add_customer(cfd); break;
        case 2: modify_customer_details(cfd); break;
        case 3: view_assigned_loans(cfd, id); break;
        case 4: process_loan(cfd, id); break;
        case 5: approve_reject_loan(cfd, id); break;
        case 6: view_passbook(cfd); break;
        case 7: change_password(cfd, EMPLOYEE_FILE, id, sizeof(Employee), offsetof(Employee, password)); break;
        case 8: return MENU_LOGOUT;
        case 9: return MENU_EXIT;
        }
    }
}
