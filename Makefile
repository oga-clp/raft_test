CC = cc

PROG1 = raft_server
SRCS1 = raft_server.c raft_common.c
SRCS2 = raft_client.c raft_common.c

PROG2 = raft_client
OBJS1 = raft_server.o raft_common.o
OBJS2 = raft_client.o raft_common.o

.PHONY: all
all: $(PROG1) $(PROG2) clean

$(PROG1): $(OBJS1)
	$(CC) -o $@ $(OBJS1)
$(OBJS1): $(SRCS1)
	$(CC) -c $(SRCS1)
$(OBJS1): raft_server.h

$(PROG2): $(OBJS2)
	$(CC) -o $@ $(OBJS2)
$(OBJS2): $(SRCS2)
	$(CC) -c $(SRCS2)
$(OBJS2): raft_server.h

.PHONY: debug
debug: $(PROG1) $(PROG2) clean

$(PROG1): $(OBJS1)
	$(CC) -o $@ $(OBJS1)
$(OBJS1): $(SRCS1)
	$(CC) -g3 -c $(SRCS1) -DDEBUG
$(OBJS1): raft_server.h

$(PROG2): $(OBJS2)
	$(CC) -o $@ $(OBJS2)
$(OBJS2): $(SRCS2)
	$(CC) -g3 -c $(SRCS2) -DDEBUG
$(OBJS2): raft_server.h

.PHONY: clean
clean:
	rm -f $(OBJS1) $(OBJS2)