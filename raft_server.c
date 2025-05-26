#include "raft_server.h"

/* Persistent state on all services */
int					currentTerm = 0;
char				votedFor[NODE_NAME_LEN] = {0};
LOG_ENTRIES_INFO	*log = NULL;

/* Volatile state on all servers */
int					commitIndex = 0;
int					lastApplied = 0;

int main(int argc, char *argv[])
{
	int				i, ret, result;
	int				myrole;
	int				mysock, target_sock;
	int				sock_nonblock;
	int				el_timeout;
	int				hb_interval;
	int				node_num;
	int				voted;
	int				send_requestvote;
	RPC_INFO		buf;
	NODE_INFO		*nodes, *pt_node, *tmp_node;
	NODE_INFO		mynode;
	struct timespec	ts, last_ts;

	/* Initialization */
	ret = RET_SUCCESS;
	el_timeout = DEFAULT_ELECTION_TIMEOUT;
	hb_interval = DEFAULT_HEARTBEAT_INTERVAL;
	node_num = 0;
	voted = 0;
	send_requestvote = 0;
	mysock = -1;
	target_sock = -1;
	myrole = ROLE_FOLLOWER;
	nodes = NULL;
	pt_node = NULL;
	tmp_node = NULL;
	memset(&mynode, 0, sizeof(mynode));
	result = set_timeout(&last_ts);
	if (result != RET_SUCCESS) {
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	}

	/* Check arguments */
	if (argc != 2) {
		print_msg("Usage: %s node_name", argv[0]);
		ret = RET_ERR_INVALID_ARG;
		goto exit;
	}

	/* Read a cluster configuration */
	ret = get_config(&el_timeout, &nodes, &node_num);
	if (ret != RET_SUCCESS) {
		print_msg("Error: get_config() fails. (ret=%d)", ret);
		goto exit;
	}

	/* Get my node configuration */
	strncpy(mynode.name, argv[1], sizeof(mynode.name) - 1);
	pt_node = nodes;
	while (pt_node) {
		if (!strcmp(mynode.name, pt_node->name)) {
			mynode = *pt_node;
			break;
		}
		pt_node = pt_node->next;
	}
	if (mynode.port == 0) {
		print_msg("Error: %s does not exist in a cluster configuration file.", argv[1]);
		ret = RET_ERR_INVALID_ARG;
		goto exit;
	}

	/* Debug */
	print_msg("election timeout: %d", el_timeout);
	pt_node = nodes;
	while (pt_node) {
		if (!strcmp(mynode.name, pt_node->name)) {
			print_msg("name: %s, port: %d *", pt_node->name, pt_node->port);
		} else {
			print_msg("name: %s, port: %d", pt_node->name, pt_node->port);
		}
		pt_node = pt_node->next;
	}

	/* Prepare for communication */
	mysock = socket(AF_INET, SOCK_DGRAM, 0);
	if (mysock == -1) {
		print_msg("Error: socket() failed. (errno=%d)", errno);
		ret = RET_ERR_SOCKET;
		goto exit;
	}
	if (bind(mysock, (const struct sockaddr *)&mynode.addr, sizeof(mynode.addr)) == -1) {
		print_msg("Error: bind() failed. (errno=%d)", errno);
		ret = RET_ERR_BIND;
		goto exit;
	}
	sock_nonblock = 1;
	ioctl(mysock, FIONBIO, &sock_nonblock);

	/* Start communication */
	while (1) {
		int 				size;
		struct sockaddr_in	tmp_addr;
		socklen_t 			tmp_addrlen = sizeof(struct sockaddr_in);

		/* Receive */
		memset(&buf, 0, sizeof(buf));
		size = recvfrom(mysock, &buf, sizeof(RPC_INFO), 0, (struct sockaddr *)&tmp_addr, &tmp_addrlen);

		if (size == -1) {
			if (errno == EAGAIN) {
				// print_msg("No data");
			} else {
				print_msg("Error: recvfrom() failed. (errno=%d)", errno);
			}
		} else {
			/* Received data */
			switch (buf.type) {
			case RPC_TYPE_APPEND_ENTRIES_REQ:
				if ((myrole == ROLE_CANDIDATE && buf.append_req.term >= currentTerm) ||
						(myrole != ROLE_CANDIDATE && buf.append_req.term > currentTerm)) {
					/* Should be demoted */
					/* If I am a CANDIDATE, the same term means that a new LEADER emerges. */
					currentTerm = buf.append_req.term;
					ret = init_follower(&myrole, &last_ts);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() fails. (ret=%d)", ret);
						goto exit;
					}
				}

				if (myrole != ROLE_LEADER) {
					/* Reset election timeout */
					result = set_timeout(&last_ts);
					if (result != RET_SUCCESS) {
						ret = RET_ERR_CLOCK_GETTIME;
						goto exit;
					}
				}
				
				if (!strlen(buf.append_req.entries)) {
					/* Heartbeat */
					print_msg("Received Heartbeat from %s", buf.name);
				} else {
					/* AppendEntries RPC */
					print_msg("Received %s from %s", buf.append_req.entries, buf.name);
				}
				break;
			case RPC_TYPE_REQUEST_VOTE_REQ:
				pt_node = nodes;
				while (pt_node) {
					if (!strcmp(buf.request_req.candidateId, pt_node->name)) {
						break;
					}
					pt_node = pt_node->next;
				}
				if (!pt_node) {
					/* Invalid node name */
					break;
				}

				if (buf.request_req.term > currentTerm) {
					/* Should be demoted */
					currentTerm = buf.request_req.term;
					ret = init_follower(&myrole, &last_ts);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() fails. (ret=%d)", ret);
						goto exit;
					}
				}

				if (myrole == ROLE_FOLLOWER) {
					target_sock = socket(AF_INET, SOCK_DGRAM, 0);
					RPC_INFO packet;
					memset(&packet, 0, sizeof(packet));
					packet.type = RPC_TYPE_REQUEST_VOTE_RES;
					strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
					packet.request_res.term = currentTerm;

					/* Check if candidate's term is new */
					if (buf.request_req.term == currentTerm) {
						if (votedFor[0] == '\0' || !strcmp(buf.request_req.candidateId, votedFor)) {
							/* Vote */
							packet.request_res.voteGranted = RAFT_TRUE;
							strncpy(votedFor, buf.request_req.candidateId, sizeof(votedFor) - 1);
						}
					} else {
						/* Cadidate's term is old, not vote */
						packet.request_res.voteGranted = RAFT_FALSE;
					}

					/* 要修正 lastLogIndex, lastLotTermチェック*/

					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);
					print_msg("Send RequestVote RPC response to %s", pt_node->name);
				}
				break;
			case RPC_TYPE_INSTALL_SNAPSHOT_REQ:
				break;
			case RPC_TYPE_APPEND_ENTRIES_RES:
				if (myrole == ROLE_LEADER && buf.append_res.term > currentTerm) {
					/* Should be demoted */
					currentTerm = buf.append_res.term;
					ret = init_follower(&myrole, &last_ts);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() fails. (ret=%d)", ret);
						goto exit;
					}
				}

				/* 要修正 ログチェック*/

				break;
			case RPC_TYPE_REQUEST_VOTE_RES:
				if (buf.request_res.term > currentTerm) {
					/* Should be demoted */
					currentTerm = buf.request_res.term;
					ret = init_follower(&myrole, &last_ts);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() fails. (ret=%d)", ret);
						goto exit;
					}
				}

				/* Get vote */
				if (myrole == ROLE_CANDIDATE) {
					if (buf.request_res.term == currentTerm && buf.request_res.voteGranted) {
						voted++;
					}
				}
				break;
			case RPC_TYPE_INSTALL_SNAPSHOT_RES:
				break;
			default:
				break;
			}
		}

		if (myrole == ROLE_FOLLOWER) {
			/* Check election timeout */
			result = check_timeout(&last_ts, el_timeout);
			if (result == RET_ERR_EXCEED_TIMEOUT) {
				print_msg("Role switched from FOLLOWER to CANDIDATE.");
				myrole = ROLE_CANDIDATE;
				currentTerm++;
				/* Vote for itself  */
				voted = 1;
				send_requestvote = 0;
			} else if (result == RET_ERR_CLOCK_GETTIME) {
				ret = RET_ERR_CLOCK_GETTIME;
				goto exit;
			}
		}

		if (myrole == ROLE_CANDIDATE) {
			/* Check votes obtained */
			if (voted > node_num / 2 + 1) {
				print_msg("Role switched from CANDIDATE to LEADER.");
				result = set_timeout(&last_ts);
				if (result != RET_SUCCESS) {
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}
				myrole = ROLE_LEADER;
			} else {
				/* Check election timeout */
				result = check_timeout(&last_ts, el_timeout);
				if (result == RET_ERR_EXCEED_TIMEOUT) {
					print_msg("Election timeout. Restart election.");
					currentTerm++;
					/* Vote for itself  */
					voted = 1;
					send_requestvote = 0;
				} else if (result == RET_ERR_CLOCK_GETTIME) {
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}

				/* Send RequestVote RPC to all servers */
				if (!send_requestvote) {
					pt_node = nodes;
					while (pt_node) {
						if (!strcmp(mynode.name, pt_node->name)) {
							pt_node = pt_node->next;
							continue;
						}
						target_sock = socket(AF_INET, SOCK_DGRAM, 0);
						
						RPC_INFO packet;
						memset(&packet, 0, sizeof(packet));
						packet.type = RPC_TYPE_REQUEST_VOTE_REQ;
						strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
						packet.request_req.term = currentTerm;
						strncpy(packet.request_req.candidateId, mynode.name, sizeof(packet.request_req.candidateId) - 1);
						packet.request_req.lastLogIndex = 0; //要修正
						packet.request_req.lastLogTerm = 0; //要修正
						sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
						close(target_sock);
						print_msg("Send RequestVote RPC request to %s", pt_node->name);

						pt_node = pt_node->next;
					}
				}
				send_requestvote = 1;
			}
		}

		if (myrole == ROLE_LEADER) {
			/* Check heartbeat send interval */
			result = check_timeout(&last_ts, hb_interval);
			if (result == RET_SUCCESS) {
				continue;
			}

			/* Send Heartbeat to all servers */
			pt_node = nodes;
			while (pt_node) {
				if (!strcmp(mynode.name, pt_node->name)) {
					pt_node = pt_node->next;
					continue;
				}
				target_sock = socket(AF_INET, SOCK_DGRAM, 0);
				
				/* Send a heartbeat packet */
				RPC_INFO packet;
				memset(&packet, 0, sizeof(packet));
				packet.type = RPC_TYPE_APPEND_ENTRIES_REQ;
				strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
				packet.append_req.term = currentTerm;
				strncpy(packet.append_req.leaderId, mynode.name, sizeof(packet.append_req.leaderId) - 1);
				packet.append_req.prevLogIndex = 0; //要修正
				packet.append_req.prevLogTerm = 0; //要修正
				packet.append_req.leaderCommit = 0; //要修正
				sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
				//sendto(target_sock, "HELLO", 5, 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
				close(target_sock);
				print_msg("Send Heartbeat to %s", pt_node->name);

				pt_node = pt_node->next;
			}
		}
	}

exit:
	/* Free memory */
	pt_node = nodes;
	while (pt_node) {
		tmp_node = pt_node->next;
		free(pt_node);
		pt_node = tmp_node;
	}
	nodes = NULL;
	pt_node = NULL;
	tmp_node = NULL;

	/* Close sockets */
	if (mysock != -1) {
		close(mysock);
	}

	return ret;
}


int get_config(int *timeout, PNODE_INFO *nodes, int *node_num)
{
	int			i, ret, pos;
	int			get_timeout_flag;
	char		line[CONF_LINE_LEN], buf[BUF_SIZE];
	char		*pt;
	NODE_INFO	*pt_node, *tmp_node;
	FILE		*fp;

	/* Initialization */
	ret = RET_SUCCESS;
	pos = 0;
	get_timeout_flag = 0;
	fp = NULL;
	pt = NULL;
	pt_node = NULL;
	tmp_node = NULL;
	memset(line, 0, sizeof(line));
	memset(buf, 0, sizeof(buf));

	/* Open a cluster configuration file */
	fp = fopen("cluster.conf", "r");
	if (!fp) {
		print_msg("Error: cannot open a cluster configuration file.");
		ret = RET_ERR_OPEN_CONFIG_FILE;
		goto exit;
	}

	while (fgets(line, CONF_LINE_LEN, fp) != NULL) {
		if (get_timeout_flag == 0) {
			/* Read a timeout value */
			*timeout = atoi(line);
			get_timeout_flag++;
			continue;
		}

		/* Allocate memory spaces for a node information */
		*node_num++;
		tmp_node = (NODE_INFO *)malloc(sizeof(NODE_INFO));
		if (!tmp_node) {
			print_msg("Error: malloc() failed. (errno=%d)", errno);
			ret = RET_ERR_MALLOC;
			goto exit;
		}
		
		/* Read a node name*/
		pt = line;
		pos = 0;
		while (pt[pos] != ' ') {
			tmp_node->name[pos] = pt[pos];
			pos++;
		}
		
		/* Read a port number */
		pos++;
		i = 0;
		while (pt[pos] != '\n' && pt[pos] != '\0') {
			buf[i] = pt[pos];
			i++;
			pos++;
		}
		tmp_node->port = atoi(buf);
		tmp_node->next = NULL;

		/* Create address information */
		tmp_node->addr.sin_family = AF_INET;
		tmp_node->addr.sin_port = htons((unsigned short)tmp_node->port);
		tmp_node->addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

		if (*nodes == NULL) {
			*nodes = tmp_node;
		} else {
			pt_node = *nodes;
			while (pt_node->next != NULL) {
				pt_node = pt_node->next;
			}
			pt_node->next = tmp_node;
		}
	}

exit:
	/* Close a cluster configuration file */
	if (fp) {
		fclose(fp);
		fp = NULL;
	}

	return ret;
}

int check_timeout(struct timespec *last_ts, int timeout_sec)
{
	int ret;
	int result;
	int randTime;
	struct timespec ts;

	/* Initialization */
	ret = RET_SUCCESS;

	/* Randomized timeout */
	srand((unsigned)time(NULL));
	randTime = rand() % DEFAULT_RANDOMIZED_TIMEOUT;

	result = clock_gettime(CLOCK_MONOTONIC, &ts);
	if (result == -1) {
		print_msg("Error: clock_gettime() failed. (errno=%d)", errno);
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	} else {
		if ((ts.tv_sec + (double)ts.tv_nsec / 1000000000) - (last_ts->tv_sec + (double)last_ts->tv_nsec / 1000000000) - (double)randTime / 1000 > timeout_sec) {
			*last_ts = ts;
			ret = RET_ERR_EXCEED_TIMEOUT;
			goto exit;
		}
	}

exit:
	return ret;
}

int set_timeout(struct timespec *last_ts)
{
	int ret;
	int result;

	/* Initialization */
	ret = RET_SUCCESS;

	result = clock_gettime(CLOCK_MONOTONIC, last_ts);
	if (result == -1) {
		print_msg("Error: clock_gettime() failed. (errno=%d)", errno);
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	}

exit:
	return ret;
}

int init_follower(int *myrole, struct timespec *last_ts)
{
	int		ret = RET_SUCCESS;
	int		result;
	char	roleBefore[ROLE_LEN] = {0};

	result = get_role(*myrole, roleBefore);
	if (result) {
		ret = result;
		goto exit;
	}

	*myrole = ROLE_FOLLOWER;
	memset(votedFor, 0, sizeof(votedFor));
	ret = set_timeout(last_ts);

	if (strcmp(roleBefore, "FOLLOWER")) {
		print_msg("Role switched from %s to FOLLOWER.", roleBefore);
	}

exit:
	return ret;
}

int get_role(int role, char *roleStr)
{
	int		ret = RET_SUCCESS;
	
	switch (role) {
	case ROLE_FOLLOWER:
		strncpy(roleStr, "FOLLOWER", ROLE_LEN);
		break;
	case ROLE_CANDIDATE:
		strncpy(roleStr, "CANDIDATE", ROLE_LEN);	
		break;
	case ROLE_LEADER:
		strncpy(roleStr, "LEADER", ROLE_LEN);
		break;
	default:
		print_msg("Error: Invalid role (%d)", role);
		ret = RET_ERR_INVALID_ROLE;
		goto exit;
	}

exit:
	return ret;
}

void print_msg(char *fmt, ...)
{
	int			result;
	time_t		now;
	struct tm	*now_local;
	char		*msg[MSG_LEN] = {0};
	va_list		argp;

	va_start(argp, fmt);
	vsnprintf((char *)msg, MSG_LEN, fmt, argp);
	va_end(argp);

	now = time(NULL);
	if (now == -1) {
		printf("Error: time() failed. (errno=%d) TERM%3d: %s\n", errno, currentTerm, msg);
		return;
	}

	now_local = localtime(&now);
	printf("%04d/%02d/%02d %02d:%02d:%02d TERM%3d: %s\n",
		now_local->tm_year + 1900, now_local->tm_mon + 1, now_local->tm_mday, now_local->tm_hour, now_local->tm_min, now_local->tm_sec,
		currentTerm, msg);
	return;
}