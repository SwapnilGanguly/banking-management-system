/* Administrator role. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bank.h"

static void add_employee(int cfd)
{
    Employee e = {0};
    int role;
    if (!ask_name(cfd, "First name: ", e.first_name, 0)) return;
    if (!ask_name(cfd, "Last name: ", e.last_name, 0)) return;
    if (!ask_new_password(cfd, "Initial password: ", &e.password)) return;
    if (!ask_int(cfd, "Role (1. Employee  2. Manager): ", &role)) return;
    if (role != 1 && role != 2) { send_str(cfd, "Invalid role.\n"); return; }
    e.role = role == 1 ? ROLE_EMPLOYEE : ROLE_MANAGER;
    e.active = 1;

    int id = append_rec(EMPLOYEE_FILE, &e, sizeof e);
    if (id < 0) send_str(cfd, "Could not add employee.\n");
    else sendf(cfd, "%s added. Login ID: %d\n", role_name(e.role), id);
}

static int show_employee(int cfd, int id, Employee *e)
{
    if (load_rec(EMPLOYEE_FILE, id, e, sizeof *e) < 0) { send_str(cfd, "Employee not found.\n"); return 0; }
    sendf(cfd, "\nEmployee ID : %d\nName        : %s %s\nRole        : %s\nStatus      : %s\n",
          e->id, e->first_name, e->last_name, role_name(e->role), e->active ? "ACTIVE" : "DEACTIVATED");
    return 1;
}

static void modify_employee_details(int cfd)
{
    int id;
    Employee probe;
    if (!ask_int(cfd, "Employee ID: ", &id) || !show_employee(cfd, id, &probe)) return;

    char first[NAME_LEN], last[NAME_LEN], yn[8];
    if (!ask_name(cfd, "New first name (Enter to keep): ", first, 1)) return;
    if (!ask_name(cfd, "New last name (Enter to keep): ", last, 1)) return;
    ask(cfd, "Reset password? (y/n): ", yn, sizeof yn);
    unsigned long long pw = 0;
    int reset = (yn[0] == 'y' || yn[0] == 'Y');
    if (reset && !ask_new_password(cfd, "New password: ", &pw)) return;

    int fd = db_open(EMPLOYEE_FILE);
    Employee e;
    int ok = 0;
    lock_rec(fd, id, sizeof e, F_WRLCK);
    if (read_rec(fd, id, &e, sizeof e) == 0) {
        if (first[0]) strcpy(e.first_name, first);
        if (last[0]) strcpy(e.last_name, last);
        if (reset) e.password = pw;
        ok = write_rec(fd, id, &e, sizeof e) == 0;
    }
    unlock_rec(fd, id, sizeof e);
    close(fd);
    send_str(cfd, ok ? "Employee details updated.\n" : "Update failed.\n");
}

static void modify_details(int cfd)
{
    int choice;
    if (!ask_int(cfd, "Modify 1. Customer  2. Employee : ", &choice)) return;
    if (choice == 1) modify_customer_details(cfd);
    else if (choice == 2) modify_employee_details(cfd);
    else send_str(cfd, "Invalid choice.\n");
}

/* Loans still waiting on this employee; such an employee cannot be promoted. */
static int open_loans_of(int emp_id)
{
    int fd = db_open(LOAN_FILE), count = 0;
    lock_all(fd, F_RDLCK);
    int n = rec_count(fd, sizeof(Loan));
    for (int i = 1; i <= n; i++) {
        Loan l;
        if (read_rec(fd, i, &l, sizeof l) == 0 && l.employee_id == emp_id &&
            (l.status == LOAN_ASSIGNED || l.status == LOAN_PROCESSING))
            count++;
    }
    unlock_all(fd);
    close(fd);
    return count;
}

static void manage_roles(int cfd)
{
    int id, choice;
    Employee probe;
    if (!ask_int(cfd, "Employee ID: ", &id) || !show_employee(cfd, id, &probe)) return;
    if (!ask_int(cfd, "1. Set role Employee  2. Set role Manager  3. Activate  4. Deactivate : ", &choice)) return;
    if (choice < 1 || choice > 4) { send_str(cfd, "Invalid choice.\n"); return; }
    if (choice == 2 && probe.role == ROLE_EMPLOYEE) {
        int open = open_loans_of(id);
        if (open) { sendf(cfd, "Employee still has %d open loan application(s); cannot promote.\n", open); return; }
    }
    /* A role change takes effect at the user's next login. */
    int fd = db_open(EMPLOYEE_FILE);
    Employee e;
    int ok = 0;
    lock_rec(fd, id, sizeof e, F_WRLCK);
    if (read_rec(fd, id, &e, sizeof e) == 0) {
        if (choice == 1) e.role = ROLE_EMPLOYEE;
        if (choice == 2) e.role = ROLE_MANAGER;
        if (choice == 3) e.active = 1;
        if (choice == 4) e.active = 0;
        ok = write_rec(fd, id, &e, sizeof e) == 0;
    }
    unlock_rec(fd, id, sizeof e);
    close(fd);
    if (ok) sendf(cfd, "Updated: %s %s is now %s (%s).\n", e.first_name, e.last_name, role_name(e.role), e.active ? "ACTIVE" : "DEACTIVATED");
    else send_str(cfd, "Update failed.\n");
}

static void list_all_users(int cfd)
{
    StrBuf sb = {0};
    int fd, n;

    sb_printf(&sb, "\n--- Administrators ---\n%-6s %s\n", "ID", "Name");
    fd = db_open(ADMIN_FILE);
    lock_all(fd, F_RDLCK);
    n = rec_count(fd, sizeof(Admin));
    for (int i = 1; i <= n; i++) {
        Admin a;
        if (read_rec(fd, i, &a, sizeof a) == 0) sb_printf(&sb, "%-6d %s %s\n", a.id, a.first_name, a.last_name);
    }
    unlock_all(fd);
    close(fd);

    sb_printf(&sb, "\n--- Employees & Managers ---\n%-6s %-30s %-9s %s\n", "ID", "Name", "Role", "Status");
    fd = db_open(EMPLOYEE_FILE);
    lock_all(fd, F_RDLCK);
    n = rec_count(fd, sizeof(Employee));
    for (int i = 1; i <= n; i++) {
        Employee e;
        char name[2 * NAME_LEN + 2];
        if (read_rec(fd, i, &e, sizeof e) < 0) continue;
        snprintf(name, sizeof name, "%s %s", e.first_name, e.last_name);
        sb_printf(&sb, "%-6d %-30s %-9s %s\n", e.id, name, role_name(e.role), e.active ? "ACTIVE" : "DEACTIVATED");
    }
    if (n == 0) sb_printf(&sb, "(none)\n");
    unlock_all(fd);
    close(fd);

    sb_printf(&sb, "\n--- Customers ---\n%-6s %-30s %-11s %14s\n", "A/C", "Name", "Status", "Balance");
    fd = db_open(CUSTOMER_FILE);
    lock_all(fd, F_RDLCK);
    n = rec_count(fd, sizeof(Customer));
    for (int i = 1; i <= n; i++) {
        Customer c;
        char name[2 * NAME_LEN + 2];
        if (read_rec(fd, i, &c, sizeof c) < 0) continue;
        snprintf(name, sizeof name, "%s %s", c.first_name, c.last_name);
        sb_printf(&sb, "%-6d %-30s %-11s %14.2f\n", c.id, name, c.active ? "ACTIVE" : "DEACTIVATED", c.balance);
    }
    if (n == 0) sb_printf(&sb, "(none)\n");
    unlock_all(fd);
    close(fd);

    sb_printf(&sb, "\nPasswords are stored only as hashes and cannot be displayed; use 'Modify Details' to reset one.\n");
    sb_send(cfd, &sb);
}

int admin_menu(int cfd, int id)
{
    static const char *const items[] = {
        "Add New Bank Employee", "Modify Customer/Employee Details", "Manage User Roles",
        "View All Users", "Change Password", "Logout", "Exit",
    };
    for (;;) {
        switch (menu_choice(cfd, "Administrator Menu", items, 7)) {
        case 1: add_employee(cfd); break;
        case 2: modify_details(cfd); break;
        case 3: manage_roles(cfd); break;
        case 4: list_all_users(cfd); break;
        case 5: change_password(cfd, ADMIN_FILE, id, sizeof(Admin), offsetof(Admin, password)); break;
        case 6: return MENU_LOGOUT;
        case 7: return MENU_EXIT;
        }
    }
}
