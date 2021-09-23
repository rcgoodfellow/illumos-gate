#
# Copyright 2019 Joyent, Inc.
# Copyright 2022 Oxide Computer Co.
#

CC=gcc
CFLAGS=-Wall -W -Wextra -Werror
SRC_ROOT=$(PWD)/../..

TARGETS=builder
OBJS=builder.o file_cp.o users.o

BUILD_USERS =	./build_users_c.sh

all: $(TARGETS)

builder: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<

users.c: $(BUILD_USERS) $(ROOT)/etc/passwd $(ROOT)/etc/group
	$(BUILD_USERS) $(ROOT) > users.c.tmp && mv users.c.tmp users.c

clean:
	rm -f $(TARGETS) $(OBJS) users.c
