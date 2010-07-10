/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include "shared_func.h"
#include "logger.h"
#include "sockopt.h"
#include "fast_task_queue.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_global.h"
#include "storage_service.h"
#include "storage_nio.h"

static void client_sock_read(int sock, short event, void *arg);
static void client_sock_write(int sock, short event, void *arg);
static int trigger_nio_recv(struct fast_task_info *pTask);

void task_finish_clean_up(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;

	pClientInfo = (StorageClientInfo *)pTask->arg;

	memset(pTask->arg, 0, sizeof(StorageClientInfo));

	free_queue_push(pTask);
}

void recv_notify_read(int sock, short event, void *arg)
{
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;
	long task_addr;
	int bytes;
	int result;

	while (1)
	{
		if ((bytes=read(sock, &task_addr, sizeof(task_addr))) < 0)
		{
			if (!(errno == EAGAIN || errno == EWOULDBLOCK))
			{
				logError("file: "__FILE__", line: %d, " \
					"call read failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, strerror(errno));
			}

			break;
		}
		else if (bytes == 0)
		{
			break;
		}

		pTask = (struct fast_task_info *)task_addr;
		pClientInfo = (StorageClientInfo *)pTask->arg;

		if (pClientInfo->sock < 0)  //quit flag
		{
			struct storage_thread_data *pThreadData;
			struct timeval tv;

			pThreadData = g_thread_data + pClientInfo->thread_index;
                        tv.tv_sec = 1;
                        tv.tv_usec = 0;
			event_base_loopexit(pThreadData->ev_base, &tv);
			return;
		}

		switch (pClientInfo->stage)
		{
			case FDFS_STORAGE_STAGE_NIO_RECV:
				result = trigger_nio_recv(pTask);
				break;
			default:
				logError("file: "__FILE__", line: %d, " \
					"invalid stage: %d", __LINE__, \
					pClientInfo->stage);
				result = EINVAL;
				break;
		}

		if (result != 0)
		{
			task_finish_clean_up(pTask);
			close(pClientInfo->sock);
		}
	}
}

static int trigger_nio_recv(struct fast_task_info *pTask)
{
	int result;
	StorageClientInfo *pClientInfo;
	struct storage_thread_data *pThreadData;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pThreadData = g_thread_data + pClientInfo->thread_index;
	event_set(&pTask->ev_read, pClientInfo->sock, EV_READ, \
			client_sock_read, pTask);
	if ((result=event_base_set(pThreadData->ev_base, &pTask->ev_read)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"event_base_set fail.", __LINE__);
		return result;
	}

	event_set(&pTask->ev_write, pClientInfo->sock, EV_WRITE, \
			client_sock_write, pTask);
	if ((result=event_base_set(pThreadData->ev_base, \
					&pTask->ev_write)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"event_base_set fail.", __LINE__);
		return result;
	}

	if ((result=event_add(&pTask->ev_read, &g_network_tv)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"event_add fail.", __LINE__);
		return result;
	}

	return 0;
}

int send_add_event(struct fast_task_info *pTask)
{
	pTask->offset = 0;

	/* direct send */
	client_sock_write(pTask->ev_write.ev_fd, EV_WRITE, pTask);

	return 0;
}

static void client_sock_read(int sock, short event, void *arg)
{
	int bytes;
	int recv_bytes;
	struct fast_task_info *pTask;
        StorageClientInfo *pClientInfo;

	pTask = (struct fast_task_info *)arg;

	if (event == EV_TIMEOUT)
	{
		if (pTask->offset == 0 && pTask->req_count > 0)
		{
			if (event_add(&pTask->ev_read, &g_network_tv) != 0)
			{
				close(pTask->ev_read.ev_fd);
				task_finish_clean_up(pTask);

				logError("file: "__FILE__", line: %d, " \
						"event_add fail.", __LINE__);
			}
		}
		else
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, recv timeout, " \
				"recv offset: %d, expect length: %d", \
				__LINE__, pTask->client_ip, \
				pTask->offset, pTask->length);

			close(pTask->ev_read.ev_fd);
			task_finish_clean_up(pTask);
		}

		return;
	}

        pClientInfo = (StorageClientInfo *)pTask->arg;

	while (1)
	{
		if (pClientInfo->total_length == 0) //recv header
		{
			recv_bytes = sizeof(TrackerHeader) - pTask->offset;
		}
		else
		{
			recv_bytes = pTask->length - pTask->offset;
		}

		bytes = recv(sock, pTask->data + pTask->offset, recv_bytes, 0);
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				if(event_add(&pTask->ev_read, &g_network_tv)!=0)
				{
					close(pTask->ev_read.ev_fd);
					task_finish_clean_up(pTask);

					logError("file: "__FILE__", line: %d, "\
						"event_add fail.", __LINE__);
				}
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, strerror(errno));

				close(pTask->ev_read.ev_fd);
				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logDebug("file: "__FILE__", line: %d, " \
				"client ip: %s, recv failed, " \
				"connection disconnected.", \
				__LINE__, pTask->client_ip);

			close(pTask->ev_read.ev_fd);
			task_finish_clean_up(pTask);
			return;
		}

		if (pClientInfo->total_length == 0) //header
		{
			if (pTask->offset + bytes < sizeof(TrackerHeader))
			{
				if (event_add(&pTask->ev_read, &g_network_tv)!=0)
				{
					close(pTask->ev_read.ev_fd);
					task_finish_clean_up(pTask);

					logError("file: "__FILE__", line: %d, "\
						"event_add fail.", __LINE__);
				}

				pTask->offset += bytes;
				return;
			}

			pClientInfo->total_length=buff2long(((TrackerHeader *) \
						pTask->data)->pkg_len);
			if (pClientInfo->total_length < 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, pkg length: " \
					INT64_PRINTF_FORMAT" < 0", \
					__LINE__, pTask->client_ip, \
					pClientInfo->total_length);

				close(pTask->ev_read.ev_fd);
				task_finish_clean_up(pTask);
				return;
			}

			pClientInfo->total_length += sizeof(TrackerHeader);
			if (pClientInfo->total_length > pTask->size)
			{
				pTask->length = pTask->size;
			}
			else
			{
				pTask->length = pClientInfo->total_length;
			}
		}

		pTask->offset += bytes;
		if (pTask->offset >= pTask->length) //recv done
		{
			pTask->req_count++;

			if (pClientInfo->total_offset == 0)
			{
				pClientInfo->total_offset = pTask->offset;
				storage_deal_task(pTask);
			}
			else
			{
				pClientInfo->total_offset += pTask->offset;
				//to do ...
			}

			return;
		}
	}

	return;
}

static void client_sock_write(int sock, short event, void *arg)
{
	int bytes;
	int result;
	struct fast_task_info *pTask;

	pTask = (struct fast_task_info *)arg;
	if (event == EV_TIMEOUT)
	{
		logError("file: "__FILE__", line: %d, " \
			"send timeout", __LINE__);

		close(pTask->ev_write.ev_fd);
		task_finish_clean_up(pTask);

		return;
	}

	while (1)
	{
		bytes = send(sock, pTask->data + pTask->offset, \
				pTask->length - pTask->offset,  0);
		//printf("%08X sended %d bytes\n", (int)pTask, bytes);
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				if (event_add(&pTask->ev_write, &g_network_tv) != 0)
				{
					close(pTask->ev_write.ev_fd);
					task_finish_clean_up(pTask);

					logError("file: "__FILE__", line: %d, " \
						"event_add fail.", __LINE__);
				}
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, strerror(errno));

				close(pTask->ev_write.ev_fd);
				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"send failed, connection disconnected.", \
				__LINE__);

			close(pTask->ev_write.ev_fd);
			task_finish_clean_up(pTask);
			return;
		}

		pTask->offset += bytes;
		if (pTask->offset >= pTask->length)
		{
			pTask->offset = 0;
			pTask->length  = 0;

			if ((result=event_add(&pTask->ev_read, \
						&g_network_tv)) != 0)
			{
				close(pTask->ev_read.ev_fd);
				task_finish_clean_up(pTask);

				logError("file: "__FILE__", line: %d, "\
					"event_add fail.", __LINE__);
				return;
			}

			return;
		}
	}
}
