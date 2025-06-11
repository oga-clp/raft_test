#include "raft_server.h"


int main(int argc, char *argv[])
{
	int				ret, result;
	int				el_timeout;
	int				node_num;
	int				mysock, target_sock;
	int				sock_nonblock;
	char			command_type[COMMAND_TYPE_LEN];
	char			command_option[COMMAND_LEN];
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
	nodes = NULL;
	pt_node = NULL;
	tmp_node = NULL;
	memset(&mynode, 0, sizeof(mynode));
	memset(command_type, 0, sizeof(command_type));
	memset(command_option, 0, sizeof(command_option));

	/* Check arguments */
	if (argc < 2 || argc > 3) {
		print_msg_client("Usage: %s command [option]", argv[0]);
		ret = RET_ERR_INVALID_ARG;
		goto exit;
	}

	/* Get command type */
	if (!strcmp(argv[1], COMMAND_TYPE_GET)) {
		strncpy(command_type, COMMAND_TYPE_GET, COMMAND_TYPE_LEN);
	} else if (!strcmp(argv[1], COMMAND_TYPE_SET)) {
		strncpy(command_type, COMMAND_TYPE_SET, COMMAND_TYPE_LEN);

		/* Get option */
		if (argc != 3) {
			print_msg_client("Usage: %s command [option]", argv[0]);
			ret = RET_ERR_INVALID_ARG;
			goto exit;
		}
		strncpy(command_option, argv[2], sizeof(command_option));
	} else {
		print_msg_client("Error: Invalid command type.");
		ret = RET_ERR_INVALID_COMMAND;
		goto exit;
	}

	/* Read a cluster configuration */
	ret = get_config_client(&el_timeout, &nodes, &node_num);
	if (ret) {
		print_msg_client("Error: get_config() failed. (ret=%d)", ret);
		goto exit;
	}

	/* Debug */
	print_msg_client("election timeout: %d", el_timeout);
	pt_node = nodes;
	while (pt_node) {
		print_msg_client("name: %s, port: %d", pt_node->name, pt_node->port);
		pt_node = pt_node->next;
	}

	/* Create address information */
	strncpy(mynode.name, DEFAULT_CLIENT_NAME, sizeof(mynode.name) - 1);
	mynode.port = DEFAULT_CLIENT_PORT;
	mynode.next = NULL;
	mynode.addr.sin_family = AF_INET;
	mynode.addr.sin_port = htons((unsigned short)mynode.port);
	mynode.addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

	/* Prepare for communication */
	mysock = socket(AF_INET, SOCK_DGRAM, 0);
	if (mysock == -1) {
		print_msg_client("Error: socket() failed. (errno=%d)", errno);
		ret = RET_ERR_SOCKET;
		goto exit;
	}
	if (bind(mysock, (const struct sockaddr *)&mynode.addr, sizeof(mynode.addr)) == -1) {
		print_msg_client("Error: bind() failed. (errno=%d)", errno);
		ret = RET_ERR_BIND;
		goto exit;
	}
	sock_nonblock = 1;
	ioctl(mysock, FIONBIO, &sock_nonblock);

	/* Create a packet */
	RPC_INFO packet;
	memset(&packet, 0, sizeof(packet));
	if (!strcmp(command_type, COMMAND_TYPE_GET)) {
		// 要修正
	} else if (!strcmp(command_type, COMMAND_TYPE_SET)) {
		packet.type = RPC_TYPE_SET_COMMAND_REQ;
		strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
		strncpy(packet.setcommand_req.command, command_option, sizeof(packet.setcommand_req.command) - 1);
	}

	/* Start communication */
	int received = RAFT_FALSE;
	pt_node = nodes;
	while (pt_node) {
		/* Send a command request */
		target_sock = socket(AF_INET, SOCK_DGRAM, 0);
		sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
		close(target_sock);
		print_msg_client("Send request to %s", pt_node->name);

		/* Receive command result*/
		result = set_timeout_client(&last_ts);
		while (1) {
			int 				size;
			struct sockaddr_in	tmp_addr;
			socklen_t 			tmp_addrlen = sizeof(struct sockaddr_in);
			
			memset(&buf, 0, sizeof(buf));
			size = recvfrom(mysock, &buf, sizeof(RPC_INFO), 0, (struct sockaddr *)&tmp_addr, &tmp_addrlen);
			if (size == -1) {
				if (errno == EAGAIN) {
					// print_msg("No data");
				} else {
					print_msg_client("Error: recvfrom() failed. (errno=%d)", errno);
					ret = RET_ERR_RECVFROM;
					goto exit;
				}
			} else {
				/* Received the data */
				received = RAFT_TRUE;
				break;
			}

			result = check_timeout_client(&last_ts, DEFAULT_CLIENT_TIMEOUT);
			if (result == RET_ERR_EXCEED_TIMEOUT) {
				print_msg_client("Error: command timeout.");
				break;
			} else if (result == RET_ERR_CLOCK_GETTIME) {
				print_msg_client("Error] check_timeout_client() failed. (ret=%d)", result);
				ret = RET_ERR_CLOCK_GETTIME;
				goto exit;
			}
		}
		
		if (received == RAFT_TRUE) {
			break;
		}

		pt_node = pt_node->next;
	}

	if (received == RAFT_FALSE) {
		print_msg_client("Error: command failed.");
		ret = RET_ERR_COMMAND;
		goto exit;
	}

	if (!strcmp(command_type, COMMAND_TYPE_GET)) {
		/* Print the data */

	} else if (!strcmp(command_type, COMMAND_TYPE_SET)) {
		if (!buf.setcommand_res.committed) {
			/* Not leader, retry */
			received = RAFT_FALSE;

			pt_node = nodes;
			while (pt_node) {
				if (!strcmp(buf.setcommand_res.leaderId, pt_node->name)) {
					break;
				}
				pt_node = pt_node->next;
			}
			if (!pt_node) {
				/* Invalid node name */
				print_msg_client("Error: response includes invalid node name %s.", buf.name);
				ret = RET_ERR_INVALID_NODE_NAME;
				goto exit;
			}

			/* Send a command request */
			target_sock = socket(AF_INET, SOCK_DGRAM, 0);
			sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
			close(target_sock);
			print_msg_client("Send request to %s (redirected)", pt_node->name);

			/* Receive command result*/
			result = set_timeout_client(&last_ts);
			while (1) {
				int 				size;
				struct sockaddr_in	tmp_addr;
				socklen_t 			tmp_addrlen = sizeof(struct sockaddr_in);
				
				memset(&buf, 0, sizeof(buf));
				size = recvfrom(mysock, &buf, sizeof(RPC_INFO), 0, (struct sockaddr *)&tmp_addr, &tmp_addrlen);
				if (size == -1) {
					if (errno == EAGAIN) {
						// print_msg("No data");
					} else {
						print_msg_client("Error: recvfrom() failed. (errno=%d)", errno);
						ret = RET_ERR_RECVFROM;
						goto exit;
					}
				} else {
					/* Received the data */
					if (buf.setcommand_res.committed) {
						received = RAFT_TRUE;
					}
					break;
				}

				result = check_timeout_client(&last_ts, DEFAULT_CLIENT_TIMEOUT);
				if (result == RET_ERR_EXCEED_TIMEOUT) {
					print_msg_client("Error: command timeout.");
					break;
				} else if (result == RET_ERR_CLOCK_GETTIME) {
					print_msg_client("Error: check_timeout_client() failed. (ret=%d)", result);
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}
			}
		}
	}

	if (received == RAFT_FALSE) {
		print_msg_client("Error: command failed.");
		ret = RET_ERR_COMMAND;
		goto exit;
	} else {
		print_msg_client("Success.");
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

int get_config_client(int *timeout, PNODE_INFO *nodes, int *node_num)
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
		print_msg_client("Error: cannot open a cluster configuration file.");
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
			print_msg_client("Error: malloc() failed. (errno=%d)", errno);
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

int check_timeout_client(struct timespec *last_ts, int timeout_sec)
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
		print_msg_client("Error: clock_gettime() failed. (errno=%d)", errno);
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	} else {
		if ((ts.tv_sec + (double)ts.tv_nsec / 1000000000) - (last_ts->tv_sec + (double)last_ts->tv_nsec / 1000000000) - (double)randTime / 1000 > timeout_sec) {
			*last_ts = ts;
			ret = RET_ERR_EXCEED_TIMEOUT;
			//print_msg("rand time: %f sec", (double)randTime / 1000);
			goto exit;
		}
	}

exit:
	return ret;
}

int set_timeout_client(struct timespec *last_ts)
{
	int ret;
	int result;

	/* Initialization */
	ret = RET_SUCCESS;

	result = clock_gettime(CLOCK_MONOTONIC, last_ts);
	if (result == -1) {
		print_msg_client("Error: clock_gettime() failed. (errno=%d)", errno);
		ret = RET_ERR_CLOCK_GETTIME;
		goto exit;
	}

exit:
	return ret;
}

void print_msg_client(char *fmt, ...)
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
		printf("Error: time() failed. (errno=%d) %s\n", errno, msg);
		return;
	}

	now_local = localtime(&now);
	printf("%04d/%02d/%02d %02d:%02d:%02d %s\n",
		now_local->tm_year + 1900, now_local->tm_mon + 1, now_local->tm_mday, now_local->tm_hour, now_local->tm_min, now_local->tm_sec, msg);
	return;
}