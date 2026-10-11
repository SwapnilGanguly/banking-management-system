# Banking Management System — CS-513 System Software Mini Project

A concurrent client–server banking system in C. Data is stored in binary files on the
server. Concurrency is controlled with `fcntl()` record locks, one process per client,
and a System V semaphore.

## Build & run

```bash
make                 # builds ./server and ./client
./server             # terminal 1 (default port 8085; ./server <port> to change)
./client             # terminal 2..n  (./client <ip> <port> for another host)
make test            # automated end-to-end + concurrency test
make reset           # wipe all bank data (data/ directory)
```

On first start the server creates the administrator **ID 1 / password `admin123`**.
Typical first steps: admin → add an employee and a manager → employee → add customers.

All users log in with their numeric ID (shown when the account is created) and password.

## Features by role

| Role | Menu |
|---|---|
| Customer | View balance, Deposit, Withdraw, Transfer, Apply for loan, View my loans, Change password, Add feedback, Transaction history, Logout, Exit |
| Employee | Add customer, Modify customer, View assigned loans, Process loan, Approve/Reject loan, Customer passbook, Change password, Logout, Exit |
| Manager | Activate/Deactivate customer, Assign loan to employee, Review feedback, Change password, Logout, Exit |
| Admin | Add employee/manager, Modify customer/employee details, Manage roles (promote/demote/activate/deactivate), View all users, Change password, Logout, Exit |

Loan lifecycle: `PENDING` (customer applies) → `ASSIGNED` (manager) → `PROCESSING` (employee picks it up) → `APPROVED` (amount credited to balance) / `REJECTED`.

## Source layout

| File | Contents (class-diagram counterpart) |
|---|---|
| `src/server.c` | `Bank`: socket setup, `fork` per client, login (`validate_login`), dispatch to role menus |
| `src/client.c` | Thin terminal client (hides password input with `termios`) |
| `src/db.c` | Record I/O, `fcntl` locking, `get_next_id` (`append_rec`), `log_transaction`, session locks |
| `src/net.c` | Socket send/receive and prompt helpers |
| `src/ops.c` | Shared operations: login check, passbook, modify customer, change password |
| `src/customer.c`, `employee.c`, `manager.c`, `admin.c` | `Customer`, `Employee`, `Manager`, `Admin` |
| `src/bank.h` | Record structs: `Customer`, `Employee` (also managers), `Admin`, `Loan`, `Feedback`, `Transaction` |

## System calls used

- **Sockets:** `socket`, `setsockopt`, `bind`, `listen`, `accept`, `connect`, `read`, `write`
- **Processes:** `fork`, `_exit`, `getpid`, `sigaction` (`SA_NOCLDWAIT` → no zombies, `SIGPIPE` ignored)
- **Files:** `open`, `pread`, `pwrite`, `fstat`, `fdatasync`, `close`, `mkdir`
- **Locking:** `fcntl(F_SETLKW / F_SETLK)` with `F_RDLCK` / `F_WRLCK` / `F_UNLCK`
- **Semaphores (IPC):** `semget`, `semctl`, `semop` with `IPC_NOWAIT | SEM_UNDO`
- **Terminal:** `tcgetattr` / `tcsetattr` (client password echo off)

## Concurrency, synchronization and ACID

**Why fork instead of threads:** `fcntl` locks belong to a process. Threads in the same
process would never block each other on these locks. With a separate process per client,
the kernel enforces mutual exclusion between clients.

**Record-level locking.** Each record is fixed-size and stored at offset `(id-1)*sizeof(record)`.
So an operation locks only the bytes of the record it touches:
- reads (balance, details) take a shared `F_RDLCK`, so many readers can run at once;
- updates (deposit, withdraw, password, status) take an exclusive `F_WRLCK` around read → modify → write;
- creating a record locks the whole file briefly, so two clients never get the same new ID;
- listings (passbook, pending loans) take a shared lock on the whole file. The output is built in memory and sent after unlocking, so a slow client never holds a lock.

**Transfers** lock both accounts before reading either one. The locks are always taken in
ascending account-number order, so A→B and B→A running at the same time cannot deadlock.
If the second write fails, the first is rolled back.

**Loan approval** locks the loan record, then the customer record. No operation locks them
in the reverse order, so there is no deadlock cycle. The status is re-checked under the
lock, so a loan cannot be approved twice. For the same reason, two managers cannot assign
one loan to two different employees.

**One session per user.** At login the handler takes a non-blocking write lock on byte `id`
of a per-role session file. A second login finds the byte locked and is refused. The kernel
releases the lock automatically on logout or when the process dies, including on a client crash.

**Client limit.** A System V counting semaphore initialised to `MAX_CLIENTS` (64). Each
handler takes one unit with `SEM_UNDO`, so the unit is given back however the process ends.

**ACID summary**
- *Atomicity:* multi-record updates run fully inside the lock section and roll back on a write
  error. No socket I/O happens inside a critical section, so a client disconnecting mid-operation
  cannot leave a half-applied update.
- *Consistency:* balance checks (no overdraft, active accounts only, valid loan state) are done under the lock.
- *Isolation:* exclusive record locks serialize conflicting operations; readers use shared locks.
- *Durability:* every write is followed by `fdatasync()` before success is reported.
- *Limitation:* there is no write-ahead journal. A power cut between the two writes of a transfer is not recovered.

**Passwords** are stored as salted 64-bit FNV-1a hashes, not plain text. This is not a
cryptographic hash; a real system would use bcrypt or argon2.

## Test (`tests/concurrency_test.sh`)

The test starts a fresh server in a temp directory. 10 customers then log in at the same
time and make 150 transfers, including opposite transfers between the same pair of accounts.
The script checks:
- every balance is exact and total money is conserved;
- the transaction log has one entry per transfer leg;
- a second login of the same user is refused;
- the full loan flow works: apply → assign → approve → credited.
