#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <fcntl.h>

#define RAFT_FALSE						0
#define RAFT_TRUE						1

#define RET_SUCCESS					0
#define RET_ERR_INVALID_ARG			1
#define RET_ERR_OPEN_CONFIG_FILE	2
#define RET_ERR_MALLOC				3
#define RET_ERR_SOCKET				4
#define RET_ERR_BIND				5
#define RET_ERR_CLOCK_GETTIME		6
#define RET_ERR_EXCEED_TIMEOUT		7
#define RET_ERR_INVALID_ROLE		8
#define RET_ERR_OPEN_FILE			9
#define RET_ERR_STAT				10
#define RET_ERR_INVALID_FILE		11
#define RET_ERR_FTRUNCATE			12
#define RET_ERR_INVALID_COMMAND		13
#define RET_ERR_RECVFROM			14
#define RET_ERR_COMMAND				15
#define RET_ERR_INVALID_NODE_NAME	16

#define BUF_SIZE					256
#define CONF_LINE_LEN				1024
#define NODE_NAME_LEN				32
#define COMMAND_LEN					128
#define TERM_LEN					32
#define LOG_LINE_LEN				COMMAND_LEN + TERM_LEN
#define MSG_LEN						4096
#define ROLE_LEN					10
#define FILENAME_LEN				128
#define COMMAND_TYPE_LEN			8

#define DEFAULT_ELECTION_TIMEOUT	5
#define DEFAULT_RANDOMIZED_TIMEOUT	500	// milliseconds
#define DEFAULT_HEARTBEAT_INTERVAL	1
#define SERVER_ADDR					"127.0.0.1"
#define LOG_FILENAME_POSTFIX			"_log.dat"
#define VOTEDFOR_FILENAME_POSTFIX		"_votedFor.dat"
#define CURRENTTERM_FILENAME_POSTFIX	"_currentTerm.dat"

#define RPC_TYPE_APPEND_ENTRIES_REQ		0
#define RPC_TYPE_REQUEST_VOTE_REQ		1
#define RPC_TYPE_INSTALL_SNAPSHOT_REQ	2
#define RPC_TYPE_GET_COMMAND_REQ		3
#define RPC_TYPE_SET_COMMAND_REQ		4

#define RPC_TYPE_APPEND_ENTRIES_RES		10
#define RPC_TYPE_REQUEST_VOTE_RES		11
#define RPC_TYPE_INSTALL_SNAPSHOT_RES	12
#define RPC_TYPE_GET_COMMAND_RES		13
#define RPC_TYPE_SET_COMMAND_RES		14

#define ROLE_FOLLOWER					0
#define ROLE_CANDIDATE					1
#define ROLE_LEADER						2

#define DEFAULT_CLIENT_TIMEOUT		3
#define DEFAULT_CLIENT_PORT			3100
#define DEFAULT_CLIENT_NAME			"client"

#define COMMAND_TYPE_GET				"GET"
#define COMMAND_TYPE_SET				"SET"

typedef struct _NODE_INFO {
	char				name[NODE_NAME_LEN];
	int					port;
	int					nextIndex;
	int					matchIndex;
	struct sockaddr_in 	addr;
	struct _NODE_INFO	*next;
} NODE_INFO, *PNODE_INFO;

typedef struct _LOG_INFO {
	int		term;
	char	command[COMMAND_LEN];
} LOG_INFO, *PLOG_INFO;

typedef struct _LOG_ENTRIES_INFO {
	int							index;
	struct _LOG_INFO			log;
	struct _LOG_ENTRIES_INFO	*next;
} LOG_ENTRIES_INFO, *PLOG_ENTRIES_INFO;

typedef struct _APPEND_ENTRIES_REQ {
	int					term;
	char				leaderId[NODE_NAME_LEN];
	int					prevLogIndex;
	int					prevLogTerm;
	char				entries[COMMAND_LEN]; // まずは単一コマンドのみ実装する。
	int					leaderCommit;
} APPEND_ENTRIES_REQ, *PAPPEND_ENTRIES_REQ;

typedef struct _REQUEST_VOTE_REQ {
	int					term;
	char				candidateId[NODE_NAME_LEN];
	int					lastLogIndex;
	int					lastLogTerm;
} REQUEST_VOTE_REQ, *PREQUEST_VOTE_REQ;

typedef struct _GET_COMMAND_REQ {
	int					nop;	// 一旦最後にコミットされたログエントリを取得するだけ。nopの値は何でもよい。
} GET_COMMAND_REQ, *PGET_COMMAND_REQ;

typedef struct _SET_COMMAND_REQ {
	char				command[COMMAND_LEN];
} SET_COMMAND_REQ, *PSET_COMMAND_REQ;

typedef struct _APPEND_ENTRIES_RES {
	int					term;
	int					success;
} APPEND_ENTRIES_RES, *PAPPEND_ENTRIES_RES;

typedef struct _REQUEST_VOTE_RES {
	int					term;
	int					voteGranted;
} REQUEST_VOTE_RES, *PREQUEST_VOTE_RES;

typedef struct _GET_COMMAND_RES {
	struct _LOG_INFO	log;	// 一旦最後にコミットされたログエントリを取得するだけ
} GET_COMMAND_RES, *PGET_COMMAND_RES;

typedef struct _SET_COMMAND_RES {
	int					committed;
	char				leaderId[NODE_NAME_LEN];
} SET_COMMAND_RES, *PSET_COMMAND_RES;

typedef struct _RPC_INFO {
	int			type;					// RPC type
	char		name[NODE_NAME_LEN];	// Source
	union {
		struct _APPEND_ENTRIES_REQ	append_req;
		struct _REQUEST_VOTE_REQ	request_req;
		struct _GET_COMMAND_REQ		getcommand_req;
		struct _SET_COMMAND_REQ		setcommand_req;
		struct _APPEND_ENTRIES_RES	append_res;
		struct _REQUEST_VOTE_RES	request_res;
		struct _GET_COMMAND_RES		getcommand_res;
		struct _SET_COMMAND_RES		setcommand_res;
		// 後で追加
	};
} RPC_INFO, *PRPC_INFO;

int get_config(int *timeout, PNODE_INFO *nodes, int *node_num);
int check_timeout(struct timespec *last_ts, int timeout_sec, char *id);
int set_timeout(struct timespec *last_ts);
int init_follower(int *myrole, struct timespec *last_ts, FILE **fp);
void init_leader(PNODE_INFO *nodes, NODE_INFO mynode);
int get_role(int role, char *roleStr);
int create_file(FILE **fp, char *name, char *postfix);
int read_currentTerm(FILE **fp);
int read_votedFor(FILE **fp);
int read_log(FILE **fp);
int write_currentTerm(FILE **fp);
int write_votedFor(FILE **fp);
int write_log(FILE **fp, char *command);
void print_msg(char *fmt, ...);

int get_config_client(int *timeout, PNODE_INFO *nodes, int *node_num);
int check_timeout_client(struct timespec *last_ts, int timeout_sec);
int set_timeout_client(struct timespec *last_ts);
void print_msg_client(char *fmt, ...);