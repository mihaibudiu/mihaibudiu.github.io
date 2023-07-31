/* threads.h -- user space threads */
/* $Id: threads.h,v 1.2 97/08/18 14:07:42 mihaib Exp $ */

#ifndef THREADSH
#define THREADSH

#include <setjmp.h>
#include <time.h>   /* for timed_wait */

#define PUBLIC
#define PRIVATE static
#define DEBUG_LEVEL 0
#define INTERRUPTS 0

#define MAX_THREADS 10
#define STACK_SPACE (64 * 1024)	/* in bytes */
#define TIME_QUANTA 2		/* in seconds */

typedef int thread_no_t;
typedef char * condition_t;
typedef void (*initial_function)(void);
typedef time_t time_unit;

typedef struct {
    int count;
    condition_t non_empty;
} semaphore_t;

void initialize_threads(void);
thread_no_t create_thread(initial_function); 
   /* create one thread; return a thread number */
void kill_thread(thread_no_t);
   /* kill this one thread */
void wait_cond(condition_t);
   /* wait on this condition variable */
void signal_cond(condition_t);
   /* signal on this condition variable: wake up ALL waiting threads */
void timed_wait(condition_t, time_unit how_much);
   /* wait that many time units on condition */
void start_package(thread_no_t th);
   /* start everything with this thread */
thread_no_t gettid(void);
   /* find my thread id */
int live_threads(void);
   /* how many live threads do we have? */
int ready_threads(void);
int sleeping_threads(void);

#if !INTERRUPTS
void run_thread(thread_no_t th);
   /* relinquish control to this thread */
void schedule(void);
   /* find another runnable thread */
#endif

void semaphore_init(semaphore_t *);
void P(semaphore_t *);
void V(semaphore_t *);

#endif
