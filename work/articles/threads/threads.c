/* threads.c -- user space threads implementation package */
/* $Id: threads.c,v 1.2 97/08/18 14:07:19 mihaib Exp $ */

#include "threads.h"
#include <assert.h>

#include <stdio.h>    /* panic, printf */
#include <unistd.h>   /* for sleep, alarm */
#include <signal.h>   /* for signal */
#include <time.h>     /* for time() */

struct thread {
    jmp_buf context;
    jmp_buf original_context;	/* save first context to restart after kill */
    int status;
#define THREAD_EMPTY  0
#define THREAD_READY  1
#define THREAD_DEAD   2
#define THREAD_WAITS  3
#define THREAD_ALARM  4         /* waits for some time */
#define THREAD_WAIT_ALARM 5     /* waits on condition with time limit */
    initial_function start_function;
    char *stack_start;
    condition_t waiting;	/* condition waited for */
    time_unit alarm;            /* time of awakening */
};

#define JMPBUFSIZE (sizeof(jmp_buf)/sizeof(long))

#define YES 1
#define NO  0

#if MSVC			/* Microsoft Visual C on NT jmpbuf fields*/

/* JMPBUFSIZE = 16 */

PRIVATE char *jmpbuffields[] = {
    "ebp", "ebx", "edi", "esi", "esp", "eip",
    "registration", "trylevel", "cookie", "unwindfunc", 
    "unwinddata0", "unwinddata1", "unwinddata2",
    "unwinddata3","unwinddata4", "unwinddata5"
};

#elif SUN4C			/* Sun 4 C jmpbuf fields*/

/* JMPBUFSIZE = 9 */

PRIVATE char *jmpbuffields[] = {
     "onsstack", "sigmask", 
     "sp", "pc", "npc", "psr",
     "g1", "o0", "wbcnt (sigcontext)"
};

#else  /* unknown architecture*/

#define UNK2 "field", "field"
#define UNK8 UNK2, UNK2, UNK2, UNK2
#define UNK32 UNK8, UNK8, UNK8, UNK8
PRIVATE char *jmpbuffields[] = { UNK32 }; 
       /* This should cover all possible fields*/
#undef UNK2
#undef UNK8
#undef UNK32

#endif /* architecture */

PRIVATE struct thread thread_table[MAX_THREADS];
#define for_all_threads(t) \
  for ((t) = thread_table; (t) < thread_table + MAX_THREADS; (t)++)
#define NO_THREAD (thread_table + MAX_THREADS)
PRIVATE thread_no_t current_thread;
PRIVATE jmp_buf home;		/* used for initializing*/
PRIVATE int alive;              /* live threads */
PRIVATE int sleeping;           /* for condition vars */
PRIVATE int to_alarm;           /* waiting for an alarm */

time_unit clock(void)
{
    return (time_unit) time(NULL);
}

#if INTERRUPTS
PRIVATE int handling_interrupt;	/*  true if handling interrupt */
#endif
PRIVATE int need_reschedule;
PRIVATE int kernel_mode;

PRIVATE void
panic(char * s, int d)
    /* print something and exit */
{
    fprintf(stderr, "** panic: ");
    fprintf(stderr, s, d);
    fflush(stderr);
    exit(1);
}

PRIVATE void 
jmpbuf_dump(jmp_buf *p)
    /* debugging dump of a context structure */
{
    int i;
    long *d;

    d = (long *)p;
    printf("jmp_buf structure dump:\n");
    for (i=0; i < JMPBUFSIZE; i++)
	printf("%d. %s\t= %lx\n", i, jmpbuffields[i], *d++);
}

#if DEBUG_LEVEL
PRIVATE void 
thread_dump(thread_no_t t)
{
    struct thread *p;

    printf("Dump for thread %d:\n", t);
    printf("Thread structure address %p\n", p = &thread_table[t]);
    jmpbuf_dump(&p->context);
}	
#endif

static void setup_context(int, int);

PRIVATE void 
allocate_stack(thread_no_t task_no, thread_no_t maxtasks)
    /* create space on stack for threads from task_no to maxtasks */
{
    char space[STACK_SPACE];	/* essential stack allocation*/
    
    if (task_no >= maxtasks) 
	longjmp(home, 1);	/* all threads are created*/
#if DEBUG_LEVEL
    printf("Thread %d stack begins after %p\n", 
	   task_no, &space[STACK_SPACE]);
#endif
    setup_context(task_no, maxtasks);
}

PRIVATE void 
setup_context(int task_no, int max)
/* setup the thread context */
{
    char c;

    if (setjmp(thread_table[task_no].context)) {
	if (DEBUG_LEVEL >= 2) {
	    printf("Starting now thread %d\n", task_no);
	}
	kernel_mode=NO;
	thread_table[task_no].start_function();
	
	/* if the thread reaches this point it is actually exiting;
	   kill it */
	if (DEBUG_LEVEL > 0)
	    printf("Thread %d exhausts\n", task_no);
	kill_thread(task_no);
    }	
    else {  /* Pass to create next task */
	if (DEBUG_LEVEL >= 1)
	    printf("Thread %d created\n", task_no);
	thread_table[task_no].stack_start = &c;
	allocate_stack(task_no+1, max); /* this never returns...*/
    }
}

PRIVATE void
copy_context(jmp_buf s, jmp_buf d)
/* copy a context */
{
    int i;
    char *sb, *db;

    sb = (char *) s;
    db = (char *) d;
    for (i = 0; i < sizeof(jmp_buf); i++)
	*db++ = *sb++;
}

PUBLIC void 
initialize_threads(void)
/* build the thread stacks and their context structures */
{
    struct thread* t;

    if (!setjmp(home)) 
	allocate_stack(0, MAX_THREADS);

    /* save the context in order to restore it at the top of the
       stack if the thread dies -- to recover stack space */
    for_all_threads(t) {
	copy_context(t->context, t->original_context);
    }
    alive = 0;
    sleeping = 0;
    to_alarm = 0;
}    

PRIVATE void
mark_ready(struct thread *t)
    /* mark this ready to run */
{
    assert(t);
    t->status = THREAD_READY;
    t->waiting = 0;
    t->alarm = 0;
}
	

/*****************************************************************/

PUBLIC thread_no_t 
gettid(void) 
{
    return current_thread;
}

PUBLIC int 
live_threads(void)
{
    return alive;
}

PUBLIC int 
ready_threads(void)
{
    return alive - sleeping;
}

PUBLIC int 
sleeping_threads(void)
{
    return sleeping;
}

PUBLIC thread_no_t
create_thread(initial_function start)
/* build a new thread to start here; return its id */
{
    struct thread *t;

    /* find an empty slot in the thread table */
    if (alive == MAX_THREADS) return -1;
    kernel_mode = YES;
    for_all_threads(t)
	if (t->status == THREAD_EMPTY ||
	    t->status == THREAD_DEAD) break;
    if (t == thread_table + MAX_THREADS) return -1; /* failure*/
       /* this shouldn't happen, but... */
    t->start_function = start;
    mark_ready(t);
    alive++;
    kernel_mode = NO;
#if INTERRUPTS
    if (need_reschedule) schedule();
#endif
    return t - thread_table;
}

#define legal_tid(x) ((x) >= 0 && (x) < MAX_THREADS)

PUBLIC void
kill_thread(thread_no_t th)
/* stop this thread running */
{
    struct thread *t;

    kernel_mode = YES;
    if (!legal_tid(th)) {
	kernel_mode--;
	return;
    }
    t = thread_table + th;
    if (t->status == THREAD_READY ||
	t->status == THREAD_WAITS)
	alive--;
    if (t->status == THREAD_WAITS)
	sleeping--;
    t->status = THREAD_DEAD;
    t->waiting = 0;
    if (DEBUG_LEVEL > 0) 
	printf("Thread %d died\n", th);
    copy_context(t->original_context, t->context);
    if (th == current_thread) schedule();
    kernel_mode = NO;
}

PRIVATE void
mark_and_wait(condition_t c, int state)
    /* mark current thread as indicated and make it wait on specified
       condition; schedule */
{
    struct thread *t;

    t = thread_table + current_thread;
    t->status = state;
    t->waiting = c;
    sleeping++;
    if (DEBUG_LEVEL > 0)
	printf("Thread %d sleeps on %p\n", current_thread, c);
    schedule();
}

PUBLIC void
wait_cond(condition_t c)
/* wait on a condition */
{
    kernel_mode = YES;
    mark_and_wait(c, THREAD_WAITS);
    kernel_mode = NO;
}

PUBLIC void
signal_cond(condition_t c)
/* wake up all thread waiting on this condition */
{
    struct thread * t;

    kernel_mode = YES;
    for_all_threads(t)
	if (((t->status == THREAD_WAITS) ||
	     (t->status == THREAD_WAIT_ALARM)) && 
	    (t->waiting == c))
	{
	    if (t->status == THREAD_WAIT_ALARM)
		to_alarm--;
	    mark_ready(t);
	    sleeping--;
	    if (DEBUG_LEVEL > 0) 
		printf("Thread %d woken up by %d\n", 
		       t - thread_table, current_thread);
	}
    if (need_reschedule) schedule();
    kernel_mode = NO;
}

PRIVATE void
check_alarms(void)
    /* Check to see if some threads need to be woken up */
{
    time_unit now = clock();
    struct thread * t;

    for_all_threads(t) {
	if ((t->status == THREAD_ALARM ||
	    t->status == THREAD_WAIT_ALARM) &&
	    (t->alarm < now)) {
	    if (t->status == THREAD_WAIT_ALARM) 
		sleeping--;
	    mark_ready(t);
	    if (!--to_alarm) break;
	}
    }
}

PRIVATE void
set_timer(time_unit how_much)
    /* Schedule an alarm */
{
    struct thread *t;

    t = thread_table + current_thread;
    t->alarm = clock() + how_much;
    t->status = THREAD_ALARM;
    to_alarm++;
}

PUBLIC void 
timed_wait(condition_t cond, time_unit t)
{
    set_timer(t);
    mark_and_wait(cond, THREAD_WAIT_ALARM);
}

PUBLIC void
schedule_alarm(time_unit how_much)
    /* public version */
{
    set_timer(how_much);
    schedule();
}

PUBLIC void
run_thread(thread_no_t th)
    /* relinquish control to this thread */
{
    struct thread *t, *c;

    if (! legal_tid(th)) return;
    t = thread_table + th;
    if (t->status != THREAD_READY) return;
    assert(legal_tid(current_thread));
    c = thread_table + current_thread;
    
    /* save current thread state as being here */
    if (!setjmp(c->context)) {
	if (DEBUG_LEVEL > 0) 
	    printf("Switching from %d to %d\n",
		   current_thread, th);
	current_thread = th;
	longjmp(t->context, 1);
    }
    else {
	/* resume to caller task */
	current_thread = c - thread_table;
    }
}

static int last_run = 0;  /* last thread scheduled */

PUBLIC void
schedule(void)
    /* find another runnable thread */
{
    struct thread *t;
    int i;

    need_reschedule=0;
    again:
    check_alarms();
    for (i=0; i < MAX_THREADS; i++) {
	last_run = (last_run+1) % MAX_THREADS;     /* wrap around */
	t = thread_table + last_run;
	if (t->status == THREAD_READY) break;
    }
    if (i == MAX_THREADS) {
	if (to_alarm) {
	    sleep(1);  /*!!!*/
	    goto again;
	}
	if (alive) panic("No thread runnable, %d alive\n", alive);
	else exit(0);   /* All threads dead */
    }
    if (DEBUG_LEVEL >=2)
	printf("Now scheduling %d\n", t - thread_table);
    else run_thread(t - thread_table);

    /* return to calling thread */
}

#if INTERRUPTS
PRIVATE void
interrupt(int ignored)
    /* this is the SIGALRM handler */
{
    signal(SIGALRM, interrupt);
    handling_interrupt++;
    alarm(TIME_QUANTA);
    if (handling_interrupt > 1) {
	handling_interrupt--;
	return;  /* already doing something */
    }
    if (kernel_mode) {		/*  scheduling will take place later */
	if (DEBUG_LEVEL >= 2)
	    printf("Delayed schedule; now %d\n", current_thread);
	handling_interrupt--;
	need_reschedule++;
	return;
    }
    else {
	handling_interrupt--;
	if (DEBUG_LEVEL >= 2)
	    printf("Interrupt schedule; now %d\n", current_thread);
	kernel_mode=YES;
	schedule();
	kernel_mode=NO;
    }
}
#endif

/*********** semaphore operations ***************/

PUBLIC void
semaphore_init(semaphore_t *s)
{
    if (!s) return;
    s->count = 1;
    s->non_empty = (condition_t) s;  /* unique address */
}

PUBLIC void
P(semaphore_t *s)
{
    assert(s);
    while (!s->count)
	wait_cond(s->non_empty);
    s->count--;
}

PUBLIC void 
V(semaphore_t *s)
{
    s->count++;
    signal_cond(s->non_empty);
}

PUBLIC void 
start_package(thread_no_t th)
/* start the ball rolling; almost like run_thread(), 
   but no current thread yet.
   This returns only on error. */
{
    struct thread *t;

    if (! legal_tid(th)) return;
    t = thread_table + th;
    if (t->status != THREAD_READY) return;
    last_run = th;
    current_thread = th;
#if INTERRUPTS
    signal(SIGALRM, interrupt);
    alarm(TIME_QUANTA);		/* set up interrupts */
    if (DEBUG_LEVEL >= 2) printf("set up interrupts\n");
#endif
    longjmp(t->context, 1);     /* start indicated thread */
}
