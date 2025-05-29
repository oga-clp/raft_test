#include "raft_server.h"


int main(int argc, char *argv[])
{
	int				ret;
	int				el_timeout;
	int				node_num;
	NODE_INFO		*nodes, *pt_node, *tmp_node;

	/* Initialization */
	ret = RET_SUCCESS;
	el_timeout = DEFAULT_ELECTION_TIMEOUT;
	node_num = 0;
	nodes = NULL;
	pt_node = NULL;
	tmp_node = NULL;

	/* Read a cluster configuration */
	ret = get_config_client(&el_timeout, &nodes, &node_num);
	if (ret) {
		print_msg_client("Error: get_config() failed. (ret=%d)", ret);
		goto exit;
	}

	

exit:
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