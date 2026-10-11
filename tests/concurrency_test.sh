#!/usr/bin/env bash
# End-to-end test: drives real clients against a fresh server in a temp directory.
#  1. admin creates an employee and a manager
#  2. employee opens 10 customer accounts with Rs. 1000 each, plus one empty target account
#  3. all 10 customers log in AT THE SAME TIME and transfer money: 5 x 100 into the
#     target and 5 x 10 to each neighbour in both directions (opposite transfers on the
#     same pair of accounts, which is where a deadlock would show up)
#  4. checks that no money was lost or created and that the transaction log matches
#  5. checks that one user cannot hold two sessions at once
#  6. full loan flow: apply -> manager assigns -> employee approves -> balance credited
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT=$((20000 + RANDOM % 20000))
WORK="$(mktemp -d)"
N=10
TARGET=$((N + 1))
FAIL=0

cleanup() { kill "$SERVER_PID" 2>/dev/null; wait "$SERVER_PID" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

client() { "$ROOT/client" 127.0.0.1 "$PORT"; }
check() { if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1 (expected '$3', got '$2')"; FAIL=1; fi; }
balance_of() { printf '1\n%s\ncpw%s\n1\n11\n' "$1" "$1" | client | sed -n 's/^Balance *: Rs\. //p' | head -1; }

(cd "$WORK" && exec "$ROOT/server" "$PORT" > server.log 2>&1) &
SERVER_PID=$!
sleep 0.5

# 1. admin: add employee (ID 1) and manager (ID 2)
printf '4\n1\nadmin123\n1\nRavi\nKumar\nemp1\nemp1\n1\n1\nMeera\nRao\nmgr1\nmgr1\n2\n7\n' | client > /dev/null

# 2. employee: open accounts
{
    printf '2\n1\nemp1\n'
    for i in $(seq 1 $N); do printf '1\nCust%s\nUser\ncpw%s\ncpw%s\n1000\n' "$i" "$i" "$i"; done
    printf '1\nTarget\nAccount\ncpw%s\ncpw%s\n0\n' "$TARGET" "$TARGET"
    printf '9\n'
} | client > /dev/null

# 3. concurrent transfers
for i in $(seq 1 $N); do
    next=$(( i % N + 1 )); prev=$(( (i + N - 2) % N + 1 ))
    {
        printf '1\n%s\ncpw%s\n' "$i" "$i"
        for _ in 1 2 3 4 5; do
            printf '4\n%s\n100\n' "$TARGET"
            printf '4\n%s\n10\n' "$next"
            printf '4\n%s\n10\n' "$prev"
        done
        printf '11\n'
    } | client > "$WORK/c$i.out" &
done
wait $(jobs -p | grep -v "^$SERVER_PID$") 2>/dev/null

# 4. verify
ok=$(cat "$WORK"/c*.out | grep -o 'Transferred Rs' | wc -l)
check "all $((N * 15)) concurrent transfers succeeded" "$ok" "$((N * 15))"
check "target account balance" "$(balance_of $TARGET)" "$((N * 500)).00"
for i in $(seq 1 $N); do
    b=$(balance_of "$i"); check "customer $i balance" "$b" "500.00"
done
txns=$({ printf '2\n1\nemp1\n'; for i in $(seq 1 $TARGET); do printf '6\n%s\n' "$i"; done; printf '9\n'; } \
       | client | grep -cE '^[0-9]+ +[0-9]{4}-')
check "transaction log has 2 entries per transfer + 1 per account" "$txns" "$((N * 30 + N + 1))"

# 5. one session per user: hold customer 1 logged in, then try again
mkfifo "$WORK/hold"
( client < "$WORK/hold" > /dev/null ) &
exec 3> "$WORK/hold"
printf '1\n1\ncpw1\n' >&3
sleep 0.5
second=$(printf '1\n1\ncpw1\n5\n' | client | grep -c 'already logged in')
check "second login of same customer is refused" "$second" "1"
printf '10\n5\n' >&3; exec 3>&-
sleep 0.3
again=$(printf '1\n1\ncpw1\n11\n' | client | grep -c 'Welcome, ')
check "login works again after logout" "$again" "1"

# 6. loan flow
printf '1\n2\ncpw2\n5\n2500\n11\n' | client > /dev/null
printf '3\n2\nmgr1\n2\n1\n1\n6\n' | client > /dev/null
printf '2\n1\nemp1\n4\n1\n5\n1\n1\n9\n' | client > /dev/null
check "approved loan credited" "$(balance_of 2)" "3000.00"

echo
if [ $FAIL -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; exit 1; fi
