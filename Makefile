.PHONY: all
all: raft_server clean

raft_server: raft_server.o
	cc -o raft_server raft_server.o
raft_server.o: raft_server.c
	cc -c raft_server.c
raft_server.o: raft_server.h

.PHONY: debug
debug: raft_server_d clean

raft_server_d: raft_server_d.o
	cc -o raft_server raft_server.o
raft_server_d.o: raft_server.c
	cc -g3 -c raft_server.c -DDEBUG
raft_server_d.o: raft_server.h

.PHONY: clean
clean:
	rm -f raft_server.o