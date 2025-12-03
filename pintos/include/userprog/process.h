#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stdbool.h>
#include "threads/thread.h"
#include "threads/synch.h"
#include <list.h>

struct lazy_load_aux {
    struct file *file;
    off_t ofs;
    uint32_t read_bytes;
    uint32_t zero_bytes;
};

struct sync_to_parent {
  struct list_elem elem;       /* Link in parent's children list. */
  struct semaphore sema;       /* Up when child finishes exiting. */
  struct lock lock;            /* Guards exit_code/ref_cnt/exited. */
  tid_t child_tid;             /* Child thread id. */
  int exit_code;               /* Child's exit status. */
  int ref_cnt;                 /* References from parent/child. */
  bool exited;                 /* True once child has exited. */
};

struct fork_struct {
  struct thread *parent;             /* Parent thread initiating fork. */
  struct intr_frame parent_if;       /* Snapshot of parent's registers. */
  struct semaphore semaphore;        /* Sync parent/child fork result. */
  bool success;                      /* Child setup succeeded flag. */
  struct sync_to_parent *sync2p;     /* Shared wait state with parent. */
};

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
void init_fds (struct thread *target);

#endif /* userprog/process.h */
