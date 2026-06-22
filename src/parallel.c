/*
 * POSIX threads implementation of the parallel-for helper.
 *
 * Authors:
 */

#define _GNU_SOURCE

#include "parallel.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

/* Shared state handed to every worker thread of a single parallel_for call. */
typedef struct {
    int count;
    atomic_int next;
    parallel_task_fn task;
    void *context;
} WorkQueue;

/*
 * Worker loop: repeatedly claim the next unprocessed index with an atomic
 * fetch-and-add and run the task until the queue is drained.
 */
static void *worker_main(void *arg)
{
    WorkQueue *queue = (WorkQueue *)arg;
    for (;;) {
        int index = atomic_fetch_add(&queue->next, 1);
        if (index >= queue->count) {
            break;
        }
        queue->task(queue->context, index);
    }
    return NULL;
}

void parallel_for(int count, int max_threads, parallel_task_fn task, void *context)
{
    if (count <= 0) {
        return;
    }

    int thread_count = max_threads;
    if (thread_count < 1) {
        thread_count = 1;
    }
    if (thread_count > count) {
        thread_count = count;
    }

    WorkQueue queue;
    queue.count = count;
    atomic_init(&queue.next, 0);
    queue.task = task;
    queue.context = context;

    /* A single worker means no thread creation overhead: run inline. */
    if (thread_count == 1) {
        worker_main(&queue);
        return;
    }

    pthread_t *threads = (pthread_t *)calloc((size_t)thread_count, sizeof(pthread_t));
    if (threads == NULL) {
        /* Out of memory for thread handles: fall back to running inline. */
        worker_main(&queue);
        return;
    }

    int spawned = 0;
    for (int i = 0; i < thread_count; ++i) {
        if (pthread_create(&threads[i], NULL, worker_main, &queue) == 0) {
            spawned++;
        } else {
            break;
        }
    }

    /* If no thread could be spawned, the caller still has to do the work. */
    if (spawned == 0) {
        worker_main(&queue);
    }

    for (int i = 0; i < spawned; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
}

int cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        return 1;
    }
    return (int)count;
}
