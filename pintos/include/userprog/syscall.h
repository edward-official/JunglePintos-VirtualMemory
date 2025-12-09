#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stddef.h>
#include <stdbool.h>
#include <list.h>
#include "threads/synch.h"

struct thread;
struct file;

enum fd_kind {FD_STDIN, FD_STDOUT, FD_FILE};
struct file_descriptor {
	int fd;
	struct file *file;
	struct list_elem elem;
	enum fd_kind fd_kind;
};

void syscall_init (void);
int write_handler (int fd, const void *buffer, unsigned length);
void syscall_process_cleanup (void);
bool syscall_duplicate_fds (struct thread *parent, struct thread *child);
extern struct lock filesys_lock;

#endif /* userprog/syscall.h */
