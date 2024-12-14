/* User-Level Threading Library */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "user/ulthread.h"
#include "kernel/riscv.h"

/* Standard definitions */
#include <stdbool.h>
#include <stddef.h> 

struct context {
  uint64 ra;
  uint64 sp;
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
  uint64 a0;
  uint64 a1;
  uint64 a2;
  uint64 a3;
  uint64 a4;
  uint64 a5;
};

struct ulthread {
  int thread_id;
  enum ulthread_state state;
  int priority;
  long last_scheduled_time;
  struct context context;
  uint64 stack;
};

struct ulthread scheduler_thread;
struct ulthread threads[MAXULTHREADS];
struct ulthread *current_thread = 0;
enum ulthread_scheduling_algorithm scheduling_algorithm;

int next_thread_tid = 1;
int runnable_threads = 0;

/* Get thread ID*/
int get_current_tid() {
  return current_thread->thread_id;
}

/* Thread initialization */
void ulthread_init(int schedalgo) {
  // Initialize free user level threads
  for (int i = 0; i < MAXULTHREADS; i++) {
    threads[i].thread_id = 0;
    threads[i].state = FREE;
    threads[i].priority = -1;
    threads[i].last_scheduled_time = 0;
  } 
  
  // Initialize running scheduler thread
  scheduler_thread.thread_id = 0;
  scheduler_thread.state = RUNNING;
  scheduler_thread.priority = -1;
  scheduler_thread.last_scheduled_time = 0;
  
  scheduling_algorithm = schedalgo;
}

/* Thread creation */
bool ulthread_create(uint64 start, uint64 stack, uint64 args[], int priority) {
  // Take the first free thread and assign ulthread values
  int i;
  for (i = 0; i < MAXULTHREADS; i++) {
    if (threads[i].state == FREE)
      break;
  }   
  threads[i].thread_id = next_thread_tid++;
  threads[i].priority = priority;
  threads[i].stack = stack - PGSIZE;
  threads[i].last_scheduled_time = 0;
  threads[i].state = RUNNABLE;   
  memset(&threads[i].context, 0, sizeof(threads[i].context));
  threads[i].context.ra = start;
  threads[i].context.sp = stack;
  threads[i].context.a0 = args[0];
  threads[i].context.a1 = args[1];
  threads[i].context.a2 = args[2];
  threads[i].context.a3 = args[3];
  threads[i].context.a4 = args[4];
  threads[i].context.a5 = args[5];
    
  printf("[*] ultcreate(tid: %d, ra: %p, sp: %p)\n", threads[i].thread_id, start, stack);
  runnable_threads++;
  return false;
}

/* Thread scheduler */
void ulthread_schedule(void) {
  while (runnable_threads > 0) {
    int next_scheduled_idx = -1;
    int current_scheduled_idx;
    
    for (int i = 0; i < MAXULTHREADS; i++) {
      if (threads[i].state != RUNNABLE) {
        continue;
      }        
      if (current_thread != 0 && threads[i].thread_id == current_thread->thread_id) {
        current_scheduled_idx = i;
        continue;
      }

      // Determine index of next thread according to scheduling algorithm
      switch (scheduling_algorithm) {
        case FCFS:
        case ROUNDROBIN:
          if (next_scheduled_idx == -1 || threads[i].last_scheduled_time < threads[next_scheduled_idx].last_scheduled_time) {
	    next_scheduled_idx = i;
          }
	  break;
        case PRIORITY:
          if (next_scheduled_idx == -1 || threads[i].priority > threads[next_scheduled_idx].priority) {
            next_scheduled_idx = i;
          }
      }
    }
    // Last runnable thread
    if (next_scheduled_idx == -1) {
      next_scheduled_idx = current_scheduled_idx;
    }
    
    // Context switch from scheduler to next scheduled user thread
    scheduler_thread.state = RUNNABLE;
    current_thread = &threads[next_scheduled_idx];
    current_thread->state = RUNNING;
        
    /* Add this statement to denote which thread-id is being scheduled next */
    printf("[*] ultschedule (next tid: %d)\n", current_thread->thread_id);
    ulthread_context_switch(&scheduler_thread.context, &current_thread->context);
  }  
}

/* Yield CPU time to some other thread. */
void ulthread_yield(void) {
  printf("[*] ultyield(tid: %d)\n", current_thread->thread_id);

  // ROUNDROBIN yields based on current time, FCFS doesn't yield until completion
  if (scheduling_algorithm != FCFS) {
    current_thread->last_scheduled_time = ctime();
  }

  // Context switch from currently scheduled user thread to scheduler
  current_thread->state = RUNNABLE;
  scheduler_thread.state = RUNNING;
  ulthread_context_switch(&current_thread->context, &scheduler_thread.context);
}

/* Destroy thread */
void ulthread_destroy(void) {
  current_thread->state = FREE;
  scheduler_thread.state = RUNNING;
  printf("[*] ultdestroy(tid: %d)\n", current_thread->thread_id);
  runnable_threads--;
  ulthread_context_switch(&current_thread->context, &scheduler_thread.context);
}

