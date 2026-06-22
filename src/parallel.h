/*
 * Minimal data-parallel helper built on POSIX threads.
 *
 * The collage renders many independent tiles and the video renders many
 * independent frames; both map cleanly onto a parallel-for. This helper spreads
 * a fixed number of indices across a small set of worker threads, each pulling
 * the next index from a shared atomic counter.
 *
 * Author: Pasquale Marzaioli
 */

#ifndef PARALLEL_H
#define PARALLEL_H

/*
 * Callback invoked once per index in [0, count). The same context pointer is
 * passed to every call; the callback must be safe to run concurrently for
 * different indices.
 */
typedef void (*parallel_task_fn)(void *context, int index);

/*
 * Run task(context, i) for every i in [0, count). At most max_threads worker
 * threads run at a time; the call returns only after every index has been
 * processed. With count <= 1 or a single thread the work runs on the caller.
 */
void parallel_for(int count, int max_threads, parallel_task_fn task, void *context);

/* Return the number of online logical CPUs, never less than 1. */
int cpu_count(void);

#endif /* PARALLEL_H */
