#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h" /* access to `timer_ticks` function. */
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* When a thread is not part of the sleeping queue, its `wake_up_ticks`
   member should be set to this value, to signal it not being in use. */
#define THREAD_NOT_SLEEPING (-1)

/* When the sleeping queue is empty, the `sleeping_list_min_wakeup_time_ticks`
   value should always be set to this value, to signal the list being empty. */
#define SLEEPING_QUEUE_EMPTY (-1)

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes in THREAD_BLOCKED state because they are sleeping,
   that is, processes which are waiting for a 'wakeup' event to trigger.
   It will be sorted in ascending order by `wakeup_time_ticks`. */
static struct list sleeping_list;

/* The smallest `wakeup_time_ticks` in the sleeping queue, i.e. the value
   of the first element in that list. Used for optimization: if the current
   time is less than the smallest wakeup time, then don't need to access the
   list at all.
   NOTE: When the list is empty, the value should always be `-1`. */
static int64_t sleeping_list_min_wakeup_time_ticks;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void sleeping_queue_insert_ordered(struct thread *t);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  { /* Initialize list of sleeping threads. */
    list_init (&sleeping_list);
    sleeping_list_min_wakeup_time_ticks = SLEEPING_QUEUE_EMPTY;
  }
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Returns the number of threads currently in the ready list. 
   Disables interrupts to avoid any race-conditions on the ready list. */
size_t
threads_ready (void)
{
  enum intr_level old_level = intr_disable ();
  size_t ready_thread_count = list_size (&ready_list);
  intr_set_level (old_level); 
  return ready_thread_count;
}

/* Returns the number of threads currently in the sleeping list. 
   Disables interrupts to avoid any race-conditions on the sleeping list. */
size_t
threads_sleeping (void)
{
  enum intr_level old_level = intr_disable ();
  size_t sleeping_thread_count;
  if (sleeping_list_min_wakeup_time_ticks == SLEEPING_QUEUE_EMPTY)
    return 0; /* If list is empty, no need to check size. */
  sleeping_thread_count = list_size (&sleeping_list);
  intr_set_level (old_level); 
  return sleeping_thread_count;
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;
  enum intr_level old_level;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Prepare thread for first run by initializing its stack.
     Do this atomically so intermediate values for the 'stack' 
     member cannot be observed. */
  old_level = intr_disable ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  intr_set_level (old_level);

  /* Add to run queue. */
  thread_unblock (t);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/** 
 * Makes the current thread sleep until the specified number of
 * ticks since the OS booted. Expects the current thread's 
 * `wakeup_time_ticks` member to have value `-1`.
 * 
 * @param wakeup_time_ticks the number of timer ticks since the 
 * OS booted, at which the current thread should be awoken.
 */
void thread_sleep(int64_t wakeup_time_ticks) {
  enum intr_level old_level;
  struct thread *cur;

  ASSERT(wakeup_time_ticks >= 0); /* Sanity check inputs. */
  if (wakeup_time_ticks < timer_ticks())
    return; /* There may have been context switches, so 
               return early if sleep time has already elapsed. */
  cur = thread_current(); /* Asserts that current thread's 
                             `wakeup_time_ticks` is `-1` under
                             the hood. */

  /* Disable interrupts when manipulating thread lists, 
     and when scheduling context switches. */
  ASSERT(!intr_context());
  old_level = intr_disable ();

  if (cur != idle_thread) { /* Never put idle thread to sleep. */

    /* Change the state of the caller thread to BLOCKED and store 
       the local tick to wake up, then insert into sleep queue. */
    cur->status = THREAD_BLOCKED;
    cur->wakeup_time_ticks = wakeup_time_ticks;
    sleeping_queue_insert_ordered(cur);

    /* Switch contexts, to allow for the next ready thread to run. */
    schedule();
  }

  intr_set_level (old_level);
}

/* Wake up those threads that need to be woken up, transitioning
   them into the THREAD_READY state by doing so */
void thread_wakeup() {
  enum intr_level old_level;
  struct list_elem *front_elem;
  struct thread *front_thread;
  int64_t os_timer_ticks = timer_ticks();

  /* Assert invariant of `sleeping_list_min_wakeup_time_ticks == -1`
     corresponding to the sleeping queue being empty. */
  ASSERT(sleeping_list_min_wakeup_time_ticks != SLEEPING_QUEUE_EMPTY 
         || list_empty(&sleeping_list)); /* P.S. uses implication-disjunction equivalence. */
  
  /* If the sleeping queue is empty, or if the current time is less than
     the smallest wakeup time, then no thread needs to be woken up. */
  if (sleeping_list_min_wakeup_time_ticks == SLEEPING_QUEUE_EMPTY
      || os_timer_ticks < sleeping_list_min_wakeup_time_ticks)
    return;

  /* Disable interrupts when manipulating thread lists. */
  old_level = intr_disable ();

  /* Continue popping the front of the queue, until a thread that does not need to be
     woken up is found, or until the sleeping queue becomes empty. */
  while (!list_empty(&sleeping_list)) {
    /* Peek the front of the queue, and perform sanity checks on thread state. */
    front_elem = list_front(&sleeping_list);
    front_thread = list_entry(front_elem, struct thread, elem);
    ASSERT(front_thread != NULL);
    ASSERT(front_thread->status == THREAD_BLOCKED);
    ASSERT(front_thread->wakeup_time_ticks > 0);

    /* When found first thread which doesn't need to be woken up,
       set new minimum to be the wakeup time of that thread, and break. */
    if (front_thread->wakeup_time_ticks > os_timer_ticks) {
      sleeping_list_min_wakeup_time_ticks = front_thread->wakeup_time_ticks;
      break;
    }

    /* If the thread needs to be woken up, then pop it from the front of the queue,
       configure and unblock it, and continue to the next thread in the queue. */
    list_pop_front(&sleeping_list);
    front_thread->wakeup_time_ticks = THREAD_NOT_SLEEPING;
    thread_unblock(front_thread);
  }

  /* If the above loop broke because the queue became empty,
     set the minimum to `-1`, to signal that. */
  if (list_empty(&sleeping_list)) 
    sleeping_list_min_wakeup_time_ticks = SLEEPING_QUEUE_EMPTY;

  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);
  ASSERT (t->wakeup_time_ticks == THREAD_NOT_SLEEPING); /* Running threads should not be sleeping. */

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->wakeup_time_ticks = THREAD_NOT_SLEEPING; /* Threads are not in sleeping queue on initialization */
  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Compares the `wakeup_time_ticks` value of two thread elements A and B.
   Returns true if A is less than B, or false if A is greater than or equal to B. */
static bool wakeup_time_ticks_less_than(
  const struct list_elem *a,
  const struct list_elem *b,
  void *aux UNUSED
) {
  struct thread *t_a;
  struct thread *t_b;
  
  /* Null pointer sanity checks. */
  ASSERT(a != NULL);
  ASSERT(b != NULL);
  
  /* Retrieve thread A and B from list entries. */
  t_a = list_entry(a, struct thread, elem);
  t_b = list_entry(b, struct thread, elem);

  /* Return less-than comparison. */
  return t_a->wakeup_time_ticks < t_b->wakeup_time_ticks;
}

/**
 * Inserts a thread into the sleeping queue, in ascending order of `wakeup_time_ticks`,
 * and updates the `sleeping_list_min_wakeup_time_ticks` if necessary.
 * 
 * Interrupts must be turned off, the thread should be in `THREAD_BLOCKED` state,
 * and the thread's `wakeup_time_ticks` must be positive.
 */
static void sleeping_queue_insert_ordered(struct thread *t) {
  /* Assert invariants described in above comment docs. */
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(t != NULL);
  ASSERT(t->status == THREAD_BLOCKED);
  ASSERT(t->wakeup_time_ticks > 0);

  /* Assert invariant of `sleeping_list_min_wakeup_time_ticks == -1`
     corresponding to the sleeping queue being empty. */
  ASSERT(sleeping_list_min_wakeup_time_ticks != SLEEPING_QUEUE_EMPTY 
         || list_empty(&sleeping_list)); /* P.S. uses implication-disjunction equivalence. */

  /* If the sleeping queue is empty, or if the thread's `wakeup_time_ticks` is
     less than the current minimum in the queue, push the first element to the
     front and return early. */
  if (sleeping_list_min_wakeup_time_ticks == SLEEPING_QUEUE_EMPTY 
      || t->wakeup_time_ticks <= sleeping_list_min_wakeup_time_ticks) {
    list_push_front(&sleeping_list, &t->elem);
    sleeping_list_min_wakeup_time_ticks = t->wakeup_time_ticks;
    return;
  }

  /* For any other case, insert the element in ascending order of `wakeup_time_ticks`.
     No need to adjust `sleeping_list_min_wakeup_time_ticks` as the thread's 
     `wakeup_time_ticks` greater than the current minimum in the queue. */
  list_insert_ordered(&sleeping_list, &t->elem, wakeup_time_ticks_less_than, NULL);
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);