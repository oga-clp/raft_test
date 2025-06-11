#include "raft_server.h"

/* Persistent state on all services */
int					currentTerm = 0;
char				votedFor[NODE_NAME_LEN] = {0};
LOG_ENTRIES_INFO	*log = NULL;

/* Volatile state on all servers */
int					commitIndex = 0;
int					lastApplied = 0;

char				leaderId[NODE_NAME_LEN] = {0};
LOG_ENTRIES_INFO	*log_tail = NULL;
LOG_ENTRIES_INFO	*log_lastApplied = NULL;

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
	LOG_ENTRIES_INFO	*pt_log, *tmp_log;
	struct timespec	ts, last_ts;
	FILE			*fp_log, *fp_votedFor, *fp_currentTerm, *fp_stateMachine;

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
	pt_log = NULL;
	fp_log = fp_votedFor = fp_currentTerm = fp_stateMachine = NULL;
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
	if (ret) {
		print_msg("Error: get_config() failed. (ret=%d)", ret);
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

	/* File open */
	ret = create_file(&fp_currentTerm, mynode.name, CURRENTTERM_FILENAME_POSTFIX);
	if (ret) {
		print_msg("Error: create_file() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = create_file(&fp_votedFor, mynode.name, VOTEDFOR_FILENAME_POSTFIX);
	if (ret) {
		print_msg("Error: create_file() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = create_file(&fp_log, mynode.name, LOG_FILENAME_POSTFIX);
	if (ret) {
		print_msg("Error: create_file() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = create_file(&fp_stateMachine, mynode.name, STATEMACHINE_FILENAME_POSTFIX);
	if (ret) {
		print_msg("Error: create_file() failed. (ret=%d)", ret);
		goto exit;
	}

	/* Read persistent states */
	ret = read_currentTerm(&fp_currentTerm);
	if (ret) {
		print_msg("Error: read_currentTerm() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = read_votedFor(&fp_votedFor);
	if (ret) {
		print_msg("Error: read_votedFor() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = read_log(&fp_log);
	if (ret) {
		print_msg("Error: read_log() failed. (ret=%d)", ret);
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
	pt_log = log;
	while (pt_log) {
		print_msg("I:%3d, T:%3d, %s", pt_log->index, pt_log->log.term, pt_log->log.command);
		pt_log = pt_log->next;
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

				if ((myrole == ROLE_CANDIDATE && buf.append_req.term >= currentTerm) ||
						(myrole != ROLE_CANDIDATE && buf.append_req.term > currentTerm)) {
					/* Should be demoted */
					/* If I am a CANDIDATE, the same term means that a new LEADER emerges. */
					currentTerm = buf.append_req.term;

					ret = write_currentTerm(&fp_currentTerm);
					if (ret != RET_SUCCESS) {
						print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
						goto exit;
					}

					ret = init_follower(&myrole, &last_ts, &fp_votedFor);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() failed. (ret=%d)", ret);
						goto exit;
					}
				}

				if (myrole != ROLE_FOLLOWER) {
					/* Only follower receives AppendEntries RPC */
					break;
				}

				target_sock = socket(AF_INET, SOCK_DGRAM, 0);
				RPC_INFO packet;
				memset(&packet, 0, sizeof(packet));
				packet.type = RPC_TYPE_APPEND_ENTRIES_RES;
				strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
				packet.append_res.term = currentTerm;

				if (buf.append_req.term < currentTerm) {
					packet.append_res.success = RAFT_FALSE;
					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);
					print_msg("Send Append Entries RPC response to %s (%d)", buf.name, packet.append_res.success);
					break;
				}

				/* Reset election timeout */
				result = set_timeout(&last_ts);
				if (result != RET_SUCCESS) {
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}

				/* Save leader's ID */
				strncpy(leaderId, buf.append_req.leaderId, sizeof(leaderId));

				if (!strlen(buf.append_req.entries.command)) {
					/* Heartbeat */
					print_msg("Received Heartbeat from %s. lastApplied:%d, commitIndex:%d", buf.name, lastApplied, commitIndex);
					/* Send a heartbeat response??? */
				} else {
					/* AppendEntries RPC */
					print_msg("Received AppendEntries RPC request from %s (%s)", buf.name, buf.append_req.entries.command);

					/* Debug */
					/*print_msg("leaderId:%s, prevLogIndex:%d, prevLogTerm:%d, entries(term):%d, entries(command):%s, leaderCommit:%d",
						buf.append_req.leaderId, buf.append_req.prevLogIndex, buf.append_req.prevLogTerm,
						buf.append_req.entries.term, buf.append_req.entries.command, buf.append_req.leaderCommit);
					print_msg("lastApplied:%d, commitIndex:%d", lastApplied, commitIndex);*/

					/* Check prevLogIndex and prevLogTerm */
					int tmp_term;
					LOG_ENTRIES_INFO *tmp_log_info = get_logEntry(buf.append_req.prevLogIndex);
					if (!tmp_log_info) {
						tmp_term = 0;
					} else {
						tmp_term = tmp_log_info->log.term;
					}
					if (buf.append_req.prevLogTerm != tmp_term) {
						packet.append_res.success = RAFT_FALSE;
						sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
						close(target_sock);
						print_msg("Send Append Entries RPC response to %s (%d)", buf.name, packet.append_res.success);
						break;
					}

					/* Check log conflicts */
					tmp_log_info = get_logEntry(buf.append_req.prevLogIndex + 1);
					if (tmp_log_info) {
						if (buf.append_req.entries.term != tmp_log_info->log.term) {
							/* Delete the existing entry and all that follow it */
							ret = delete_log(&fp_log, buf.append_req.prevLogIndex + 1);
							if (ret) {
								print_msg("Error: delete_log() failed. (ret=%d)", ret);
								goto exit;
							}
							/* Add a new log entry */
							ret = write_log(&fp_log, buf.append_req.entries.term, buf.append_req.entries.command);
							if (ret) {
								print_msg("Error: write_log() failed. (ret=%d)", ret);
								goto exit;
							}
						}
					} else {
						/* Add a new log entry */
						ret = write_log(&fp_log, buf.append_req.entries.term, buf.append_req.entries.command);
						if (ret) {
							print_msg("Error: write_log() failed. (ret=%d)", ret);
							goto exit;
						}
					}

					packet.append_res.success = RAFT_TRUE;
					packet.append_res.writtenIndex = get_lastLogIndex();
					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);
					print_msg("Send Append Entries RPC response to %s (%d)", buf.name, packet.append_res.success);
				}

				/* Update commitIndex */
				if (buf.append_req.leaderCommit > commitIndex) {
					commitIndex = (buf.append_req.leaderCommit > log_tail->index) ? log_tail->index : buf.append_req.leaderCommit;
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

					ret = write_currentTerm(&fp_currentTerm);
					if (ret != RET_SUCCESS) {
						print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
						goto exit;
					}

					ret = init_follower(&myrole, &last_ts, &fp_votedFor);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() failed. (ret=%d)", ret);
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
					packet.request_res.voteGranted = RAFT_FALSE;

					/* Check if candidate's term is equal or new */
					if (buf.request_req.term >= currentTerm) {
						if ((votedFor[0] == '\0' || !strcmp(buf.request_req.candidateId, votedFor)) && buf.request_req.lastLogIndex >= get_lastLogIndex()) {
							int voteFlag = 0;
							
							if (!log_tail) {
								voteFlag = 1;
							} else {
								if (buf.request_req.lastLogTerm >= log_tail->log.term) {
									voteFlag = 1;
								}
							}
							
							if (voteFlag) {
								/* Vote */
								packet.request_res.voteGranted = RAFT_TRUE;
								strncpy(votedFor, buf.request_req.candidateId, sizeof(votedFor) - 1);
								ret = write_votedFor(&fp_votedFor);
								if (ret != RET_SUCCESS) {
									print_msg("Error: write_votedFor() failed. (ret=%d)", ret);
									goto exit;
								}
								
								/* Reset election timeout */
								result = set_timeout(&last_ts);
								if (result != RET_SUCCESS) {
									ret = RET_ERR_CLOCK_GETTIME;
									goto exit;
								}
							}
						}
					}

					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);
					print_msg("Send RequestVote RPC response to %s (%d)", pt_node->name, packet.request_res.voteGranted);
				}
				break;
			case RPC_TYPE_INSTALL_SNAPSHOT_REQ:
				break;
			case RPC_TYPE_GET_COMMAND_REQ:
				break;
			case RPC_TYPE_SET_COMMAND_REQ:
				if (myrole == ROLE_LEADER) {
					/* Write data to the local log */
					ret = write_log(&fp_log, currentTerm, buf.setcommand_req.command);
					if (ret) {
						print_msg("Error: write_log() failed. (ret=%d)", ret);
						goto exit;
					}
				} else {
					/* Send response with leaderId */
					target_sock = socket(AF_INET, SOCK_DGRAM, 0);
					RPC_INFO packet;
					memset(&packet, 0, sizeof(packet));
					packet.type = RPC_TYPE_SET_COMMAND_RES;
					strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
					packet.setcommand_res.committed = RAFT_FALSE;
					strncpy(packet.setcommand_res.leaderId, leaderId, sizeof(packet.setcommand_res.leaderId) - 1);

					tmp_addr.sin_port = htons((unsigned short)DEFAULT_CLIENT_PORT);
					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&tmp_addr, tmp_addrlen);
					close(target_sock);
					print_msg("Send command response to the client (includes redirect info)");
				}
				break;
			case RPC_TYPE_APPEND_ENTRIES_RES:
				if (myrole == ROLE_LEADER && buf.append_res.term > currentTerm) {
					/* Should be demoted */
					currentTerm = buf.append_res.term;

					ret = write_currentTerm(&fp_currentTerm);
					if (ret != RET_SUCCESS) {
						print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
						goto exit;
					}

					ret = init_follower(&myrole, &last_ts, &fp_votedFor);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() failed. (ret=%d)", ret);
						goto exit;
					}
				}

				if (buf.append_res.success == RAFT_TRUE) {
					pt_node = nodes;
					while (pt_node) {
						if (!strcmp(pt_node->name, buf.name)) {
							if (buf.append_res.writtenIndex == pt_node->nextIndex) {
								pt_node->matchIndex = pt_node->nextIndex;
								pt_node->nextIndex = pt_node->nextIndex + 1;
							}
							break;
						}
						pt_node = pt_node->next;
					}
				} else {
					pt_node = nodes;
					while (pt_node) {
						if (!strcmp(pt_node->name, buf.name)) {
							if (pt_node->nextIndex - 1 > pt_node->matchIndex) {
								pt_node->nextIndex = pt_node->nextIndex - 1;
							}
							break;
						}
						pt_node = pt_node->next;
					}
				}
				break;
			case RPC_TYPE_REQUEST_VOTE_RES:
				if (buf.request_res.term > currentTerm) {
					/* Should be demoted */
					currentTerm = buf.request_res.term;

					ret = write_currentTerm(&fp_currentTerm);
					if (ret != RET_SUCCESS) {
						print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
						goto exit;
					}

					ret = init_follower(&myrole, &last_ts, &fp_votedFor);
					if (ret != RET_SUCCESS) {
						print_msg("Error: init_follower() failed. (ret=%d)", ret);
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
			result = check_timeout(&last_ts, el_timeout, mynode.name);
			if (result == RET_ERR_EXCEED_TIMEOUT) {
				print_msg("Role switched from FOLLOWER to CANDIDATE.");
				myrole = ROLE_CANDIDATE;
				memset(leaderId, 0, sizeof(leaderId));

				currentTerm++;
				ret = write_currentTerm(&fp_currentTerm);
				if (ret != RET_SUCCESS) {
					print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
					goto exit;
				}
				
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
			if (voted >= node_num / 2 + 1) {
				print_msg("Role switched from CANDIDATE to LEADER.");
				result = set_timeout(&last_ts);
				if (result != RET_SUCCESS) {
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}
				myrole = ROLE_LEADER;
				init_leader(&nodes, mynode);
			} else {
				/* Check election timeout */
				result = check_timeout(&last_ts, el_timeout, mynode.name);
				if (result == RET_ERR_EXCEED_TIMEOUT) {
					print_msg("Election timeout. Restart election.");
					currentTerm++;

					ret = write_currentTerm(&fp_currentTerm);
					if (ret != RET_SUCCESS) {
						print_msg("Error: write_currentTerm() failed. (ret=%d)", ret);
						goto exit;
					}

					/* Vote for itself  */
					voted = 1;
					send_requestvote = 0;
				} else if (result == RET_ERR_CLOCK_GETTIME) {
					ret = RET_ERR_CLOCK_GETTIME;
					goto exit;
				}

				/* Send RequestVote RPC to all servers */
				if (!send_requestvote) {
					RPC_INFO packet;
					memset(&packet, 0, sizeof(packet));
					packet.type = RPC_TYPE_REQUEST_VOTE_REQ;
					strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
					packet.request_req.term = currentTerm;
					strncpy(packet.request_req.candidateId, mynode.name, sizeof(packet.request_req.candidateId) - 1);
					packet.request_req.lastLogIndex = get_lastLogIndex();
					packet.request_req.lastLogTerm = get_lastLogTerm();
					pt_node = nodes;
					while (pt_node) {
						if (!strcmp(mynode.name, pt_node->name)) {
							pt_node = pt_node->next;
							continue;
						}
						target_sock = socket(AF_INET, SOCK_DGRAM, 0);
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
			result = check_timeout(&last_ts, hb_interval, mynode.name);
			if (result == RET_ERR_EXCEED_TIMEOUT) {
				/* Send Heartbeat to all servers */
				RPC_INFO packet;
				memset(&packet, 0, sizeof(packet));
				packet.type = RPC_TYPE_APPEND_ENTRIES_REQ;
				strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
				packet.append_req.term = currentTerm;
				strncpy(packet.append_req.leaderId, mynode.name, sizeof(packet.append_req.leaderId) - 1);
				packet.append_req.prevLogIndex = 0; // not use
				packet.append_req.prevLogTerm = 0; // not use
				packet.append_req.leaderCommit = commitIndex;
				pt_node = nodes;
				while (pt_node) {
					if (!strcmp(mynode.name, pt_node->name)) {
						pt_node = pt_node->next;
						continue;
					}
					target_sock = socket(AF_INET, SOCK_DGRAM, 0);
					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);
					print_msg("Send Heartbeat to %s", pt_node->name);

					pt_node = pt_node->next;
				}
			}

			/* Check commitIndex */
			int committed_all = 0;
			int committed = 0;
			for (pt_log = log_tail; pt_log != NULL ; pt_log = pt_log->prev) {
				if (pt_log->log.term != currentTerm) {
					break;
				}

				committed = 1; // 1 means my own commit
				pt_node = nodes;
				while (pt_node) {
					if (!strcmp(mynode.name, pt_node->name)) {
						pt_node = pt_node->next;
						continue;
					}
					if (pt_node->matchIndex >= pt_log->index) {
						committed++;
					}
					pt_node = pt_node->next;
				}

				if (committed >= node_num / 2 + 1) {
					commitIndex = pt_log->index;
					if (committed == node_num) {
						committed_all = 1;
					}
					break;
				}
			}
			
			/* Debug */
			/*print_msg("commitIndex:%d", commitIndex);
			pt_node = nodes;
			while (pt_node) {
				if (!strcmp(mynode.name, pt_node->name)) {
					pt_node = pt_node->next;
					continue;
				}
				print_msg("name:%s, matchIndex:%d, nextIndex:%d", pt_node->name, pt_node->matchIndex, pt_node->nextIndex);
				pt_node = pt_node->next;
			}*/

			/* Send AppendEntries RPC */
			if (commitIndex != get_lastLogIndex() || !committed_all) {
				pt_node = nodes;
				while (pt_node) {
					if (!strcmp(mynode.name, pt_node->name)) {
						pt_node = pt_node->next;
						continue;
					}
					if (pt_node->matchIndex == get_lastLogIndex()) {
						pt_node = pt_node->next;
						continue;
					}
					if (pt_node->nextIndex > get_lastLogIndex()) {
						pt_node = pt_node->next;
						continue;
					}

					target_sock = socket(AF_INET, SOCK_DGRAM, 0);
					RPC_INFO packet;
					LOG_ENTRIES_INFO *tmp_log_info;
					memset(&packet, 0, sizeof(packet));
					packet.type = RPC_TYPE_APPEND_ENTRIES_REQ;
					strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
					packet.append_req.term = currentTerm;
					strncpy(packet.append_req.leaderId, mynode.name, sizeof(packet.append_req.leaderId) - 1);
					packet.append_req.prevLogIndex = pt_node->nextIndex - 1;
					tmp_log_info = get_logEntry(pt_node->nextIndex - 1);
					if (!tmp_log_info) {
						packet.append_req.prevLogTerm = 0;
					} else {
						packet.append_req.prevLogTerm = tmp_log_info->log.term;
					}
					tmp_log_info = get_logEntry(pt_node->nextIndex);
					packet.append_req.entries.term = tmp_log_info->log.term;
					strncpy(packet.append_req.entries.command, tmp_log_info->log.command, sizeof(packet.append_req.entries.command) - 1);
					packet.append_req.leaderCommit = commitIndex;
					
					sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&pt_node->addr, sizeof(pt_node->addr));
					close(target_sock);

					//print_msg("Send AppendEntries RPC request to %s (%s)", pt_node->name, packet.append_req.entries.command);
					pt_node = pt_node->next;
				}
			}
		}

		/* Update state machine */
		update_stateMachine(&fp_stateMachine, myrole, mynode);
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
	while (pt_log) {
		tmp_log = pt_log->next;
		free(pt_log);
		pt_log = tmp_log;
	}
	log = NULL;

	/* Close file */
	if (fp_currentTerm) {
		fclose(fp_currentTerm);
	}
	if (fp_votedFor) {
		fclose(fp_votedFor);
	}
	if (fp_log) {
		fclose(fp_log);
	}
	if (fp_stateMachine) {
		fclose(fp_stateMachine);
	}

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
		(*node_num)++;
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

int check_timeout(struct timespec *last_ts, int timeout_sec, char *id)
{
	int ret;
	int result;
	int randTime;
	struct timespec ts;

	/* Initialization */
	ret = RET_SUCCESS;

	/* Randomized timeout */
	srand((unsigned)time(NULL) + (unsigned)id);
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
			//print_msg("rand time: %f sec", (double)randTime / 1000);
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

int init_follower(int *myrole, struct timespec *last_ts, FILE **fp)
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
	memset(leaderId, 0, sizeof(leaderId));
	memset(votedFor, 0, sizeof(votedFor));
	ret = write_votedFor(fp);
	if (ret != RET_SUCCESS) {
		print_msg("Error: write_votedFor() failed. (ret=%d)", ret);
		goto exit;
	}
	ret = set_timeout(last_ts);
	if (ret != RET_SUCCESS) {
		print_msg("Error: set_timeout() failed. (ret=%d)", ret);
		goto exit;
	}

	if (strcmp(roleBefore, "FOLLOWER")) {
		print_msg("Role switched from %s to FOLLOWER.", roleBefore);
	}

exit:
	return ret;
}

void init_leader(PNODE_INFO *nodes, NODE_INFO mynode)
{
	NODE_INFO	*pt_node = NULL;

	pt_node = *nodes;
	while (pt_node) {
		if (!strcmp(mynode.name, pt_node->name)) {
			pt_node = pt_node->next;
			continue;
		}

		pt_node->nextIndex = get_lastLogIndex() + 1;
		pt_node->matchIndex = 0;
		pt_node = pt_node->next;
	}

	return;
}

int get_role(int role, char *roleStr)
{
	int		ret = RET_SUCCESS;
	
	switch (role) {
	case ROLE_FOLLOWER:
		strncpy(roleStr, "FOLLOWER", ROLE_LEN - 1);
		break;
	case ROLE_CANDIDATE:
		strncpy(roleStr, "CANDIDATE", ROLE_LEN - 1);	
		break;
	case ROLE_LEADER:
		strncpy(roleStr, "LEADER", ROLE_LEN - 1);
		break;
	default:
		print_msg("Error: Invalid role (%d)", role);
		ret = RET_ERR_INVALID_ROLE;
		goto exit;
	}

exit:
	return ret;
}

int create_file(FILE **fp, char *name, char *postfix)
{
	int				ret = RET_SUCCESS;
	int				result;
	int				fd;
	char			file_name[FILENAME_LEN] = {0};
	struct stat		st;

	snprintf(file_name, sizeof(file_name), "dat/%s%s", name, postfix);
	result = stat(file_name, &st);
	if (result) {
		if (errno != ENOENT) {
			print_msg("Error: stat() failed. (file=%s, errno=%d)", file_name, errno);
			ret = RET_ERR_STAT;
			goto exit;
		}
		*fp = fopen(file_name, "w+");
		if (!*fp) {
			print_msg("Error: cannot open %s.", file_name);
			ret = RET_ERR_OPEN_FILE;
			goto exit;
		}
		if (!strcmp(postfix, CURRENTTERM_FILENAME_POSTFIX)) {
			fprintf(*fp, "0");
			fflush(*fp);
		}
	} else {
		*fp = fopen(file_name, "r+");
		if (!*fp) {
			print_msg("Error: cannot open %s.", file_name);
			ret = RET_ERR_OPEN_FILE;
			goto exit;
		}
		if (!strcmp(postfix, STATEMACHINE_FILENAME_POSTFIX)) {
			fd = fileno(*fp);
			result = ftruncate(fd, 0);
			if (result) {
				print_msg("Error: ftruncate() failed. (errno=%d)", errno);
				ret = RET_ERR_FTRUNCATE;
				goto exit;
			}
			rewind(*fp);
		}
	}

exit:
	return ret;
}

int read_currentTerm(FILE **fp)
{
	int		ret = RET_SUCCESS;
	int		result;

	result = fscanf(*fp, "%d", &currentTerm);
	if (!result) {
		print_msg("Error: Invalid file format (currentTerm).");
		ret = RET_ERR_INVALID_FILE;
		goto exit;
	}

exit:
	return ret;
}

int read_votedFor(FILE **fp)
{
	int		ret = RET_SUCCESS;
	int		result;
	char	tmp[NODE_NAME_LEN] = {0};

	result = fscanf(*fp, "%s", tmp);
	if (!result) {
		print_msg("Error: Invalid file format (votedFor).");
		ret = RET_ERR_INVALID_FILE;
		goto exit;
	} else {
		/* In case of content with proper format or empty */
		strncpy(votedFor, tmp, sizeof(votedFor) - 1);
	}

exit:
	return ret;
}

int read_log(FILE **fp)
{
	int					ret = RET_SUCCESS;
	int					result;
	char				line[LOG_LINE_LEN] = {0};
	LOG_ENTRIES_INFO	*tmp_log_entry = NULL;

	while (fgets(line, LOG_LINE_LEN, *fp) != NULL) {
		tmp_log_entry = (LOG_ENTRIES_INFO *)malloc(sizeof(LOG_ENTRIES_INFO));
		if (!tmp_log_entry) {
			print_msg("Error: malloc() failed. (errno=%d)", errno);
			ret = RET_ERR_MALLOC;
			goto exit;
		}

		result = sscanf(line, "%d %s", &(tmp_log_entry->log.term), tmp_log_entry->log.command);
		if (result != 2) {
			print_msg("Error: malloc() failed. (errno=%d)", errno);
			ret = RET_ERR_INVALID_FILE;
			goto exit;
		}

		tmp_log_entry->index = get_lastLogIndex() + 1;
		tmp_log_entry->next = NULL;

		if (log == NULL) {
			tmp_log_entry->prev = NULL;
			log = tmp_log_entry;
			log_tail = tmp_log_entry;
		} else {
			tmp_log_entry->prev = log_tail;
			log_tail->next = tmp_log_entry;
			log_tail = tmp_log_entry;
		}
	}

exit:
	return ret;
}

int write_currentTerm(FILE **fp)
{
	int		ret = RET_SUCCESS;
	int		result;
	int		fd;

	fd = fileno(*fp);
	result = ftruncate(fd, 0);
	if (result) {
		print_msg("Error: ftruncate() failed. (errno=%d)", errno);
		ret = RET_ERR_FTRUNCATE;
		goto exit;
	}
	rewind(*fp);

	fprintf(*fp, "%d", currentTerm);
	fflush(*fp);

exit:
	return ret;
}

int write_votedFor(FILE **fp)
{
	int		ret = RET_SUCCESS;
	int		result;
	int		fd;

	fd = fileno(*fp);
	result = ftruncate(fd, 0);
	if (result) {
		print_msg("Error: ftruncate() failed. (errno=%d)", errno);
		ret = RET_ERR_FTRUNCATE;
		goto exit;
	}
	rewind(*fp);

	fprintf(*fp, "%s", votedFor);
	fflush(*fp);

exit:
	return ret;
}

int write_log(FILE **fp, int term, char *command)
{
	int					ret = RET_SUCCESS;
	int					result;
	int					index = 0;
	char				line[LOG_LINE_LEN] = {0};
	LOG_ENTRIES_INFO	*tmp_log_entry = NULL;

	fprintf(*fp, "%d %s\n", term, command);
	fflush(*fp);

	tmp_log_entry = (LOG_ENTRIES_INFO *)malloc(sizeof(LOG_ENTRIES_INFO));
	if (!tmp_log_entry) {
		print_msg("Error: malloc() failed. (errno=%d)", errno);
		ret = RET_ERR_MALLOC;
		goto exit;
	}

	tmp_log_entry->next = NULL;
	tmp_log_entry->index = get_lastLogIndex() + 1;
	tmp_log_entry->log.term = term;
	strncpy(tmp_log_entry->log.command, command, sizeof(tmp_log_entry->log.command) - 1);

	if (log == NULL) {
		tmp_log_entry->prev = NULL;
		log = tmp_log_entry;
		log_tail = tmp_log_entry;
	} else {
		tmp_log_entry->prev = log_tail;
		log_tail->next = tmp_log_entry;
		log_tail = tmp_log_entry;
	}

	print_msg("Write to local log. T:%3d, %s", term, command);

exit:
	return ret;
}

int delete_log(FILE **fp, int index)
{
	int					ret = RET_SUCCESS;
	int					result;
	int					fd;
	long				truncate_pos = 0;
	int					cur_index = 1;
	char 				line[LOG_LINE_LEN] = {0};
	LOG_ENTRIES_INFO	*pt_log = NULL;
	LOG_ENTRIES_INFO	*tmp_log = NULL;

	rewind(*fp);
	while (fgets(line, sizeof(line), *fp)) {
		if (cur_index == index) {
			break;
		}
		truncate_pos = ftell(*fp);
		cur_index++;
	}

	fd = fileno(*fp);
	result = ftruncate(fd, truncate_pos);
	if (result) {
		print_msg("Error: ftruncate() failed. (errno=%d)", errno);
		ret = RET_ERR_FTRUNCATE;
		goto exit;
	}
	fseek(*fp, 0, SEEK_END);

	pt_log = log_tail;
	while (pt_log) {
		if (pt_log->index >= index) {
			tmp_log = pt_log->prev;
			if (tmp_log) {
				pt_log->prev->next = NULL;
			}
			free(pt_log);
			pt_log = tmp_log;
		} else {
			break;
		}
	}

	log_tail = pt_log;
	if (log_tail == NULL) {
		log = NULL;
	}

exit:
	return ret;
}

int get_lastLogIndex() {
	if (log == NULL) {
		return 0;
	} else {
		return log_tail->index;
	}
}

int get_lastLogTerm() {
	if (log == NULL) {
		return 0;
	} else {
		return log_tail->log.term;
	}
}

LOG_ENTRIES_INFO* get_logEntry(int index) {
	LOG_ENTRIES_INFO	*pt_log = NULL;

	if (index == 0) {
		return NULL;
	} else {
		pt_log = log;
		while (pt_log) {
			if (index == pt_log->index) {
				return pt_log;
			}
			pt_log = pt_log->next;
		}
		return NULL;
	}
}

void update_stateMachine(FILE **fp, int role, NODE_INFO mynode)
{
	LOG_ENTRIES_INFO	*tmp_log_entry = NULL;
	NODE_INFO			*pt_node = NULL;

	if (lastApplied == commitIndex) {
		return;
	}

	tmp_log_entry = get_logEntry(lastApplied + 1);

	while (lastApplied != commitIndex && tmp_log_entry) {
		fprintf(*fp, "%s\n", tmp_log_entry->log.command);
		fflush(*fp);

		/* Reply to client */
		if (role == ROLE_LEADER) {
			struct sockaddr_in	tmp_addr;
			socklen_t 			tmp_addrlen = sizeof(struct sockaddr_in);
			RPC_INFO			packet;
			int					target_sock;

			target_sock = socket(AF_INET, SOCK_DGRAM, 0);
			memset(&packet, 0, sizeof(packet));
			packet.type = RPC_TYPE_SET_COMMAND_RES;
			strncpy(packet.name, mynode.name, sizeof(packet.name) - 1);
			packet.setcommand_res.committed = RAFT_TRUE;
			packet.setcommand_res.index = tmp_log_entry->index;
			packet.setcommand_res.term = tmp_log_entry->log.term;
			strncpy(packet.setcommand_res.command, tmp_log_entry->log.command, sizeof(packet.setcommand_res.command) - 1);

			tmp_addr.sin_family = AF_INET;
			tmp_addr.sin_port = htons((unsigned short)DEFAULT_CLIENT_PORT);
			tmp_addr.sin_addr.s_addr = inet_addr(SERVER_ADDR);
			sendto(target_sock, &packet, sizeof(packet), 0, (struct sockaddr *)&tmp_addr, tmp_addrlen);
			close(target_sock);
			print_msg("Send command response to the client. index:%d, term:%d, command:%s", packet.setcommand_res.index, packet.setcommand_res.term, packet.setcommand_res.command);
		}

		tmp_log_entry = tmp_log_entry->next;
		lastApplied++;
	}

	return;
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