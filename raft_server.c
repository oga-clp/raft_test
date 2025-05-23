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
	int				node_num;
	RPC_INFO		buf;
	NODE_INFO		*nodes, *pt_node, *tmp_node;
	NODE_INFO		mynode;
	struct timespec	ts, last_ts;

	/* Initialization */
	ret = RET_SUCCESS;
	el_timeout = DEFAULT_ELECTION_TIMEOUT;
	node_num = 0;
	mysock = -1;
	target_sock = -1;
	myrole = ROLE_FOLLOWER;
	nodes = NULL;
	pt_node = NULL;
	tmp_node = NULL;
	memset(&mynode, 0, sizeof(mynode));
	result = clock_gettime(CLOCK_MONOTONIC, &last_ts);
	if (result == -1) {
		fprintf(stderr, "Error: clock_gettime() failed. (errno=%d)\n", errno);
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	}

	/* Check arguments */
	if (argc != 2) {
		fprintf(stderr, "Usage: %s node_name\n", argv[0]);
		ret = RET_ERR_INVALID_ARG;
		goto exit;
	}

	/* Read a cluster configuration */
	ret = get_config(&el_timeout, &nodes, &node_num);
	if (ret != RET_SUCCESS) {
		fprintf(stderr, "Error: get_config fails. (ret=%d)\n", ret);
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
		fprintf(stderr, "Error: %s does not exist in a cluster configuration file.\n", argv[1]);
		ret = RET_ERR_INVALID_ARG;
		goto exit;
	}

	/* Debug */
	printf("election timeout: %d\n", el_timeout);
	pt_node = nodes;
	while (pt_node) {
		if (!strcmp(mynode.name, pt_node->name)) {
			printf("name: %s, port: %d *\n", pt_node->name, pt_node->port);
		} else {
			printf("name: %s, port: %d\n", pt_node->name, pt_node->port);
		}
		pt_node = pt_node->next;
	}

	/* Prepare for communication */
	mysock = socket(AF_INET, SOCK_DGRAM, 0);
	if (mysock == -1) {
		fprintf(stderr, "Error: socket() failed. (errno=%d)\n", errno);
		ret = RET_ERR_SOCKET;
		goto exit;
	}
	if (bind(mysock, (const struct sockaddr *)&mynode.addr, sizeof(mynode.addr)) == -1) {
		fprintf(stderr, "Error: bind() failed. (errno=%d)\n", errno);
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
				// printf("No data\n");
			} else {
				fprintf(stderr, "Error: recvfrom() failed. (errno=%d)\n", errno);
			}
		} else {
			/* Received data */
			if (buf.type == RPC_TYPE_APPEND_ENTRIES_REQ) {
				if (!strlen(buf.append_req.entries)) {
					/* Heartbeat */
					printf("Received Heartbeat from %s\n", buf.name);
				} else {
					/* AppendEntries RPC */
					printf("Received %s from %s\n", buf.append_req.entries, buf.name);
				}
			} else if (buf.type == RPC_TYPE_REQUEST_VOTE_REQ) {
				// 要修正
			} else if (buf.type == RPC_TYPE_INSTALL_SNAPSHOT_REQ) {
				// 要修正
			} else if (buf.type == RPC_TYPE_APPEND_ENTRIES_RES) {
				// 要修正
			} else if (buf.type == RPC_TYPE_REQUEST_VOTE_RES) {
				// 要修正
			} else if (buf.type == RPC_TYPE_INSTALL_SNAPSHOT_RES) {
				// 要修正
			}
		}

		if (myrole == ROLE_FOLLOWER) {
			result = clock_gettime(CLOCK_MONOTONIC, &ts);
			if (result == -1) {
				fprintf(stderr, "Error: clock_gettime() failed. (errno=%d)\n", errno);
			} else {
				if ((ts.tv_sec + (double)ts.tv_nsec / 1000000000) - (last_ts.tv_sec + (double)last_ts.tv_nsec / 1000000000) > el_timeout) {
					printf("timeout\n");
					myrole = ROLE_CANDIDATE;
				}
			}
		}

		if (myrole == ROLE_CANDIDATE) {
			//
		}

		if (myrole == ROLE_LEADER) {
			/* Send a heartbeat to all servers */
			pt_node = nodes;
			while (pt_node) {
				if (!strcmp(mynode.name, pt_node->name)) {
					pt_node = pt_node->next;
					continue;
				}
				printf("Send start\n");
				
				target_sock = socket(AF_INET, SOCK_DGRAM, 0);
				
				/* Send a heartbeat packet */
				RPC_INFO packet;
				memset(&packet, 0, sizeof(packet));
				packet.type = RPC_TYPE_APPEND_ENTRIES_REQ;
				strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
				packet.append_req.term = 0; //要修正
				strncpy(packet.append_req.leaderId, mynode.name, sizeof(packet.append_req.leaderId) - 1); //要修正
				packet.append_req.prevLogIndex = 0; //要修正
				packet.append_req.prevLogTerm = 0; //要修正
				packet.append_req.leaderCommit = 0; //要修正
				sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
				//sendto(target_sock, "HELLO", 5, 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
				
				close(target_sock);

				printf("Send to %s\n", pt_node->name);

				pt_node = pt_node->next;
			}
			sleep(1);
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
		fprintf(stderr, "Error: cannot open a cluster configuration file.\n");
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
			fprintf(stderr, "Error: malloc() failed. (errno=%d)\n", errno);
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


