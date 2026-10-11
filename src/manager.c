/* Manager role. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

static void set_customer_status(int cfd)
{
    int id, choice;
    if (!ask_int(cfd, "Customer account number: ", &id)) return;
    Customer probe;
    if (load_rec(CUSTOMER_FILE, id, &probe, sizeof probe) < 0) { send_str(cfd, "Customer not found.\n"); return; }
    sendf(cfd, "Account %d (%s %s) is currently %s.\n", id, probe.first_name, probe.last_name,
          probe.active ? "ACTIVE" : "DEACTIVATED");
    if (!ask_int(cfd, "1. Activate  2. Deactivate : ", &choice)) return;
    if (choice != 1 && choice != 2) { send_str(cfd, "Invalid choice.\n"); return; }

    int fd = db_open(CUSTOMER_FILE);
    Customer c;
    int ok = 0;
    lock_rec(fd, id, sizeof c, F_WRLCK);
    if (read_rec(fd, id, &c, sizeof c) == 0) {
        c.active = (choice == 1);
        ok = write_rec(fd, id, &c, sizeof c) == 0;
    }
    unlock_rec(fd, id, sizeof c);
    close(fd);
    if (ok) sendf(cfd, "Account %d is now %s.\n", id, choice == 1 ? "ACTIVE" : "DEACTIVATED");
    else send_str(cfd, "Update failed.\n");
}

static int list_pending_loans(int cfd)
{
    int fd = db_open(LOAN_FILE);
    StrBuf sb = {0};
    sb_printf(&sb, "\n--- Pending loan applications ---\n%-6s %-8s %14s %s\n", "LoanID", "Cust", "Amount", "Applied on");
    int found = 0;
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Loan));
    for (int i = 1; i <= n; i++) {
        Loan l;
        if (read_rec(fd, i, &l, sizeof l) < 0 || l.status != LOAN_PENDING) continue;
        char when[32];
        format_time(l.applied_at, when, sizeof when);
        sb_printf(&sb, "%-6d %-8d %14.2f %s\n", l.id, l.customer_id, l.amount, when);
        found++;
    }
    unlock_all(fd);
    close(fd);
    if (!found) sb_printf(&sb, "(none)\n");
    sb_send(cfd, &sb);
    return found;
}

static void list_employees(int cfd)
{
    int fd = db_open(EMPLOYEE_FILE);
    StrBuf sb = {0};
    sb_printf(&sb, "\n--- Active employees ---\n%-6s %s\n", "EmpID", "Name");
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Employee));
    for (int i = 1; i <= n; i++) {
        Employee e;
        if (read_rec(fd, i, &e, sizeof e) == 0 && e.active && e.role == ROLE_EMPLOYEE)
            sb_printf(&sb, "%-6d %s %s\n", e.id, e.first_name, e.last_name);
    }
    unlock_all(fd);
    close(fd);
    sb_send(cfd, &sb);
}

static void assign_loan(int cfd)
{
    if (!list_pending_loans(cfd)) return;
    int lid, emp;
    if (!ask_int(cfd, "Loan application ID to assign: ", &lid)) return;
    list_employees(cfd);
    if (!ask_int(cfd, "Employee ID: ", &emp)) return;

    Employee e;
    if (load_rec(EMPLOYEE_FILE, emp, &e, sizeof e) < 0 || !e.active || e.role != ROLE_EMPLOYEE) {
        send_str(cfd, "That is not an active employee.\n");
        return;
    }

    /* Re-check the status under the lock: another manager may have assigned it meanwhile. */
    char msg[128] = "Loan application not found.\n";
    int fd = db_open(LOAN_FILE);
    Loan l;
    lock_rec(fd, lid, sizeof l, F_WRLCK);
    if (read_rec(fd, lid, &l, sizeof l) == 0) {
        if (l.status != LOAN_PENDING) {
            snprintf(msg, sizeof msg, "Loan #%d is no longer pending (status: %s).\n", lid, loan_status_name(l.status));
        } else {
            l.status = LOAN_ASSIGNED;
            l.employee_id = emp;
            if (write_rec(fd, lid, &l, sizeof l) == 0)
                snprintf(msg, sizeof msg, "Loan #%d assigned to %s %s (ID %d).\n", lid, e.first_name, e.last_name, emp);
        }
    }
    unlock_rec(fd, lid, sizeof l);
    close(fd);
    send_str(cfd, msg);
}

static void review_feedback(int cfd)
{
    int fd = db_open(FEEDBACK_FILE);
    StrBuf sb = {0};
    sb_printf(&sb, "\n--- Customer feedback ---\n");
    int unreviewed = 0;
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Feedback));
    for (int i = 1; i <= n; i++) {
        Feedback f;
        if (read_rec(fd, i, &f, sizeof f) < 0) continue;
        char when[32];
        format_time(f.created_at, when, sizeof when);
        sb_printf(&sb, "[#%d] %s  customer %d  %s\n      %s\n", f.id, f.reviewed ? "REVIEWED" : "NEW     ",
                  f.customer_id, when, f.message);
        unreviewed += !f.reviewed;
    }
    unlock_all(fd);
    close(fd);
    if (n == 0) sb_printf(&sb, "(no feedback)\n");
    sb_send(cfd, &sb);
    if (!unreviewed) return;

    int fid;
    if (!ask_int(cfd, "Feedback ID to mark as reviewed (0 to go back): ", &fid) || fid == 0) return;
    fd = db_open(FEEDBACK_FILE);
    Feedback f;
    int ok = 0;
    lock_rec(fd, fid, sizeof f, F_WRLCK);
    if (read_rec(fd, fid, &f, sizeof f) == 0) {
        f.reviewed = 1;
        ok = write_rec(fd, fid, &f, sizeof f) == 0;
    }
    unlock_rec(fd, fid, sizeof f);
    close(fd);
    send_str(cfd, ok ? "Marked as reviewed.\n" : "Feedback not found.\n");
}

int manager_menu(int cfd, int id)
{
    static const char *const items[] = {
        "Activate/Deactivate Customer Account", "Assign Loan Application to Employee",
        "Review Customer Feedback", "Change Password", "Logout", "Exit",
    };
    for (;;) {
        switch (menu_choice(cfd, "Manager Menu", items, 6)) {
        case 1: set_customer_status(cfd); break;
        case 2: assign_loan(cfd); break;
        case 3: review_feedback(cfd); break;
        case 4: change_password(cfd, EMPLOYEE_FILE, id, sizeof(Employee), offsetof(Employee, password)); break;
        case 5: return MENU_LOGOUT;
        case 6: return MENU_EXIT;
        }
    }
}
