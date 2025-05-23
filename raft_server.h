#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define RET_SUCCESS					0
#define RET_ERR_INVALID_ARG			1
#define RET_ERR_OPEN_CONFIG_FILE	2
#define RET_ERR_MALLOC				3
#define RET_ERR_SOCKET				4
#define RET_ERR_BIND				5
#define RET_ERR_CLOCK_GETTIME		6

#define BUF_SIZE					256
#define CONF_LINE_LEN				1024
#define NODE_NAME_LEN				32
#define COMMAND_LEN					32

#define DEFAULT_ELECTION_TIMEOUT	5
#define SERVER_ADDR					"127.0.0.1"

#define RPC_TYPE_APPEND_ENTRIES_REQ		0
#define RPC_TYPE_REQUEST_VOTE_REQ		1
#define RPC_TYPE_INSTALL_SNAPSHOT_REQ	2

#define RPC_TYPE_APPEND_ENTRIES_RES		10
#define RPC_TYPE_REQUEST_VOTE_RES		11
#define RPC_TYPE_INSTALL_SNAPSHOT_RES	12

#define ROLE_FOLLOWER					0
#define ROLE_CANDIDATE					1
#define ROLE_LEADER						2

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
	int						index;
	struct _LOG_INFO		log;
	struct _LOG_ENTRY_INFO	*next;
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

typedef struct _RPC_INFO {
	int			type;					// RPC type
	char		name[NODE_NAME_LEN];	// Source
	union {
		struct _APPEND_ENTRIES_REQ	append_req;
		struct _REQUEST_VOTE_REQ	request_req;
		// 後で追加
	};
} RPC_INFO, *PRPC_INFO;

int get_config(int *timeout, PNODE_INFO *nodes, int *node_num);