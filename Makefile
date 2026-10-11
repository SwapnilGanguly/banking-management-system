CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
SERVER_SRC = src/server.c src/net.c src/db.c src/ops.c src/customer.c src/employee.c src/manager.c src/admin.c

all: server client

server: $(SERVER_SRC) src/bank.h src/common.h
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC) -lm

client: src/client.c src/common.h
	$(CC) $(CFLAGS) -o $@ src/client.c

test: all
	./tests/concurrency_test.sh

clean:
	rm -f server client

# Deletes all bank data (accounts, transactions, ...)
reset:
	rm -rf data

.PHONY: all test clean reset
