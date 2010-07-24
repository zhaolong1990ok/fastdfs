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
#include "pthread_func.h"
#include "logger.h"
#include "sockopt.h"
#include "storage_dio.h"
#include "storage_nio.h"

static pthread_mutex_t g_dio_thread_lock;
int g_dio_thread_count = 0;

static void *dio_thread_entrance(void* arg);
 
int storage_dio_init()
{
	int result;
	struct storage_dio_thread_data *pThreadData;
	struct storage_dio_thread_data *pDataEnd;
	struct storage_dio_context *pContext;
	struct storage_dio_context *pContextEnd;
	pthread_t tid;
	pthread_attr_t thread_attr;

	if ((result=init_pthread_lock(&g_dio_thread_lock)) != 0)
	{
		return result;
	}

	if ((result=init_pthread_attr(&thread_attr, g_thread_stack_size)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"init_pthread_attr fail, program exit!", __LINE__);
		return result;
	}

	g_dio_thread_data = (struct storage_dio_thread_data *)malloc(sizeof( \
				struct storage_dio_thread_data) * g_path_count);
	if (g_dio_thread_data == NULL)
	{
		logError("file: "__FILE__", line: %d, " \
			"malloc %d bytes fail, errno: %d, error info: %s", \
			__LINE__, (int)sizeof(struct storage_dio_thread_data) * \
			g_path_count, errno, strerror(errno));
		return errno != 0 ? errno : ENOMEM;
	}

	g_dio_thread_count = 0;
	pDataEnd = g_dio_thread_data + g_path_count;
	for (pThreadData=g_dio_thread_data; pThreadData<pDataEnd; pThreadData++)
	{
		pThreadData->count=g_disk_reader_threads+g_disk_writer_threads;
		pThreadData->contexts = (struct storage_dio_context *)malloc(\
			sizeof(struct storage_dio_context) * pThreadData->count);
		if (pThreadData->contexts == NULL)
		{
			logError("file: "__FILE__", line: %d, " \
				"malloc %d bytes fail, " \
				"errno: %d, error info: %s", __LINE__, \
				(int)sizeof(struct storage_dio_context) * \
				pThreadData->count, errno, strerror(errno));
			return errno != 0 ? errno : ENOMEM;
		}
		pThreadData->reader=pThreadData->contexts;
		pThreadData->writer=pThreadData->contexts+g_disk_reader_threads;

		pContextEnd = pThreadData->contexts + pThreadData->count;
		for (pContext=pThreadData->contexts; pContext<pContextEnd; \
			pContext++)
		{
			if ((result=task_queue_init(&(pContext->queue))) != 0)
			{
				return result;
			}

			if ((result=init_pthread_lock(&(pContext->lock))) != 0)
			{
				return result;
			}

			result = pthread_cond_init(&(pContext->cond), NULL);
			if (result != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"pthread_cond_init fail, " \
					"errno: %d, error info: %s", \
					__LINE__, result, strerror(result));
				return result;
			}

			if ((result=pthread_create(&tid, &thread_attr, \
					dio_thread_entrance, pContext)) != 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"create thread failed, " \
					"startup threads: %d, " \
					"errno: %d, error info: %s", \
					__LINE__, g_dio_thread_count, \
					result, strerror(result));
				return result;
			}
			else
			{
				pthread_mutex_lock(&g_dio_thread_lock);
				g_dio_thread_count++;
				pthread_mutex_unlock(&g_dio_thread_lock);
			}
		}
	}

	pthread_attr_destroy(&thread_attr);

	return result;
}

static int dio_deal_task(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int result;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext = &(pClientInfo->file_context);

	if (pFileContext->fd < 0)
	{
		if (pFileContext->op == FDFS_STORAGE_FILE_OP_READ)
		{
			pFileContext->fd=open(pFileContext->filename, O_RDONLY);
		}
		else
		{
			pFileContext->fd = open(pFileContext->filename, \
					O_WRONLY | O_CREAT | O_TRUNC, 0644);
		}

		if (pFileContext->fd < 0)
		{
			result = errno != 0 ? errno : EACCES;
			logError("file: "__FILE__", line: %d, " \
				"open file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pFileContext->filename, \
				result, strerror(result));
			return result;
		}

		if (pFileContext->offset > 0 && lseek(pFileContext->fd, \
			pFileContext->offset, SEEK_SET) < 0)
		{
			result = errno != 0 ? errno : EIO;
			logError("file: "__FILE__", line: %d, " \
				"lseek file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pFileContext->filename, \
				result, strerror(result));
			return result;
		}
	}

	if (pFileContext->op == FDFS_STORAGE_FILE_OP_READ)
	{
		int64_t remain_bytes;
		int capacity_bytes;
		int read_bytes;

		remain_bytes = pFileContext->end - pFileContext->offset;
		capacity_bytes = pTask->size - pTask->length;
		read_bytes = (capacity_bytes < remain_bytes) ? \
				capacity_bytes : remain_bytes;

		if (read(pFileContext->fd, pTask->data + pTask->length, \
			read_bytes) != read_bytes)
		{
			result = errno != 0 ? errno : EIO;
			logError("file: "__FILE__", line: %d, " \
				"read from file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pFileContext->filename, \
				result, strerror(result));

			return result;
		}

		pTask->length += read_bytes;
		pFileContext->offset += read_bytes;
	}
	else
	{
		int write_bytes;

		write_bytes = pTask->length - pTask->offset;
		if (write(pFileContext->fd, pTask->data + pTask->offset, \
			write_bytes) != write_bytes)
		{
			result = errno != 0 ? errno : EIO;
			logError("file: "__FILE__", line: %d, " \
				"write to file: %s fail, " \
				"errno: %d, error info: %s", \
				__LINE__, pFileContext->filename, \
				result, strerror(result));

			return result;
		}

		pFileContext->offset += write_bytes;
	}

	return 0;
}

static void *dio_thread_entrance(void* arg) 
{
	int result;
	struct storage_dio_context *pContext; 
	struct fast_task_info *pTask;

	pContext = (struct storage_dio_context *)arg; 

	pthread_mutex_lock(&(pContext->lock));
	while (g_continue_flag)
	{
		if ((result=pthread_cond_wait(&(pContext->cond), \
			&(pContext->lock))) != 0)
		{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_cond_wait fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
		}

		while ((pTask=task_queue_pop(&(pContext->queue))) != NULL)
		{
			dio_deal_task(pTask);
		}
	}
	pthread_mutex_unlock(&(pContext->lock));

	if ((result=pthread_mutex_lock(&g_dio_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}
	g_dio_thread_count--;
	if ((result=pthread_mutex_unlock(&g_dio_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, strerror(result));
	}

	return NULL;
}
