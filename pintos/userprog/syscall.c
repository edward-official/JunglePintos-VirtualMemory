#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/flags.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "vm/vm.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

static void halt_handler (void) NO_RETURN;
static void exit_handler (int status) NO_RETURN;
static void exit_with_error (void) NO_RETURN;
static void validate_user_buffer (const void *buffer, size_t size);
static void validate_user_string (const char *str);
static char *copy_user_string (const char *str);
static struct file_descriptor *fd_lookup (int fd);
static int allocate_fd (struct file *file);
static void close_fd (struct file_descriptor *desc);
static int fork_handler (const char *name, struct intr_frame *f);
static int exec_handler (const char *cmd_line);
static bool create_handler (const char *file, unsigned initial_size);
static bool remove_handler (const char *file);
static int open_handler (const char *file);
static int filesize_handler (int fd);
static int read_handler (int fd, void *buffer, unsigned length);
static void seek_handler (int fd, unsigned position);
static unsigned tell_handler (int fd);
static void close_handler (int fd);
static void close_all_files (struct thread *t);
int dup2_handler (int oldfd, int newfd);
void update_fduplicated(struct thread *thread, bool b_value);

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	lock_init (&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f) {
	// user rsp 백업 
	struct thread *curr = thread_current();
    curr->u_rsp = f->rsp;
	
	switch (f->R.rax)
	{
	case SYS_HALT:
		halt_handler ();
		break;
	case SYS_EXIT:
		exit_handler ((int) f->R.rdi);
		break;
	case SYS_FORK:
		f->R.rax = fork_handler ((const char *) f->R.rdi, f);
		break;
	case SYS_EXEC:
		f->R.rax = exec_handler ((const char *) f->R.rdi);
		break;
	case SYS_WAIT:
		f->R.rax = process_wait ((tid_t) f->R.rdi);
		break;
	case SYS_CREATE:
		f->R.rax = create_handler ((const char *) f->R.rdi, (unsigned) f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove_handler ((const char *) f->R.rdi);
		break;
	case SYS_OPEN:
		f->R.rax = open_handler ((const char *) f->R.rdi);
		break;
	case SYS_FILESIZE:
		f->R.rax = filesize_handler ((int) f->R.rdi);
		break;
	case SYS_READ:
		f->R.rax = read_handler ((int) f->R.rdi, (void *) f->R.rsi, (unsigned) f->R.rdx);
		break;
	case SYS_WRITE:
		f->R.rax = write_handler ((int) f->R.rdi, (const void *) f->R.rsi, (unsigned) f->R.rdx);
		break;
	case SYS_SEEK:
		seek_handler ((int) f->R.rdi, (unsigned) f->R.rsi);
		break;
	case SYS_TELL:
		f->R.rax = tell_handler ((int) f->R.rdi);
		break;
	case SYS_CLOSE:
		close_handler ((int) f->R.rdi);
		break;
	case SYS_DUP2:
		f->R.rax = dup2_handler((int) f->R.rdi, (int) f->R.rsi);
		break;
	default:
		exit_with_error ();
	}
}

int
write_handler (int fd, const void *buffer, unsigned length) {
	if (fd < 0) return -1;
	if (length == 0) return 0;
	validate_user_buffer (buffer, length);

	// if (fd == STDOUT_FILENO) {
	// 	putbuf (buffer, length);
	// 	return (int) length;
	// }
	// if (fd == STDIN_FILENO) return -1;
	
	struct file_descriptor *desc = fd_lookup (fd);
	if (!desc) return -1;
	if (!desc->file) { /* STDIN, STDOUT */
		if (desc->fd_kind == FD_STDIN) return -1;
		if (desc->fd_kind == FD_STDOUT) {
			if (thread_current()->stdout_cnt == 0) return -1;
			putbuf (buffer, length);
			return (int) length;
		}
	}
	lock_acquire (&filesys_lock);
	int result = file_write (desc->file, buffer, length);
	lock_release (&filesys_lock);
	return result;
}

static int
read_handler (int fd, void *buffer, unsigned length) {
	if (fd < 0) return -1;
	if (length == 0) return 0;
	validate_user_buffer (buffer, length);

	// if (fd == STDIN_FILENO) {
	// 	uint8_t *dst = buffer;
	// 	for (unsigned i = 0; i < length; i++)
	// 		dst[i] = input_getc ();
	// 	return (int) length;
	// }
	// if (fd == STDOUT_FILENO)
	// 	return -1;

	struct file_descriptor *desc = fd_lookup (fd);
	if (desc == NULL) return -1;
	if (desc->file == NULL) {
		if (desc->fd_kind == FD_STDOUT) return -1;
		if (desc->fd_kind == FD_STDIN) {
			if (thread_current()->stdin_cnt == 0) return -1;
			uint8_t *dst = buffer;
			for (unsigned i = 0; i < length; i++) dst[i] = input_getc ();
			return (int) length;
		}
	}

	lock_acquire (&filesys_lock);
	int result = file_read (desc->file, buffer, length);
	lock_release (&filesys_lock);
	return result;
}

static void
halt_handler (void) {
	power_off ();
}

static void
exit_handler (int status) {
	struct thread *curr = thread_current ();
	curr->exit_status = status;
	thread_exit ();
}

static void
exit_with_error (void) {
	exit_handler (-1);
}

static void
validate_user_buffer (const void *buffer, size_t size) {
    const uint8_t *ptr = buffer;
    struct thread *curr = thread_current();
    
    /* 저장해둔 유저 rsp 가져오기 */
    uintptr_t rsp = curr->u_rsp;

    for (size_t i = 0; i < size; i++) {
        uintptr_t addr = (uintptr_t)(ptr + i);

        /* 유저 영역이 아니면 에러 */
        if (!is_user_vaddr((void *)addr)) {
            exit_with_error();
        }

        /* 페이지가 있는지 확인 */
        if (spt_find_page(&curr->spt, (void *)addr) == NULL) {
            
            /* 스택 확장 조건이라면 Page Fault 핸들러가 처리하도록 유도 */
            if (addr >= rsp - 8) {
                continue; 
            }
            
            /* 스택 확장 조건도 아니면 에러 */
            exit_with_error();
        }
    }
}

static void
validate_user_string (const char *str) {
	if (str == NULL)
		exit_with_error ();
	while (true) {
		validate_user_buffer (str, 1);
		if (*str == '\0') break;
		str++;
	}
}

static char *
copy_user_string (const char *str) {
	validate_user_string (str);
	char *copy = palloc_get_page (0);
	if (copy == NULL) return NULL;
	strlcpy (copy, str, PGSIZE);
	return copy;
}

static int
fork_handler (const char *name, struct intr_frame *f) {
	char *name_copy = copy_user_string (name);
	if (name_copy == NULL) return TID_ERROR;
	tid_t tid = process_fork (name_copy, f);
	palloc_free_page (name_copy);
	return tid;
}

static int
exec_handler (const char *cmd_line) {
	char *fn_copy = copy_user_string (cmd_line);
	if (fn_copy == NULL) return -1;
	return process_exec (fn_copy);
}

static bool
create_handler (const char *file, unsigned initial_size) {
	char *file_copy = copy_user_string (file);
	if (file_copy == NULL) return false;
	lock_acquire (&filesys_lock);
	bool success = filesys_create (file_copy, initial_size);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);
	return success;
}

static bool
remove_handler (const char *file) {
	char *file_copy = copy_user_string (file);
	if (file_copy == NULL)
		return false;
	lock_acquire (&filesys_lock);
	bool success = filesys_remove (file_copy);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);
	return success;
}

static int
open_handler (const char *file) {
	char *file_copy = copy_user_string (file);
	if (file_copy == NULL) return -1;

	lock_acquire (&filesys_lock);
	struct file *opened = filesys_open (file_copy);
	lock_release (&filesys_lock);
	palloc_free_page (file_copy);

	if (opened == NULL) return -1;

	int fd = allocate_fd (opened);
	if (fd == -1) {
		lock_acquire (&filesys_lock);
		file_close (opened);
		lock_release (&filesys_lock);
	}
	return fd;
}

static int
filesize_handler (int fd) {
	struct file_descriptor *desc = fd_lookup (fd);
	if (desc == NULL) return -1;
	lock_acquire (&filesys_lock);
	int size = file_length (desc->file);
	lock_release (&filesys_lock);
	return size;
}

static void
seek_handler (int fd, unsigned position) {
	struct file_descriptor *desc = fd_lookup (fd);
	if (!desc || !desc->file) return;
	lock_acquire (&filesys_lock);
	file_seek (desc->file, position);
	lock_release (&filesys_lock);
}

static unsigned
tell_handler (int fd) {
	struct file_descriptor *desc = fd_lookup (fd);
	if (desc == NULL)
		return 0;
	lock_acquire (&filesys_lock);
	off_t pos = file_tell (desc->file);
	lock_release (&filesys_lock);
	return (unsigned) pos;
}

static void
close_handler (int fd) {
	struct file_descriptor *desc = fd_lookup (fd);
	if (desc == NULL) return;
	close_fd (desc);
}

static struct file_descriptor *
fd_lookup (int fd) {
	struct thread *curr = thread_current ();
	if (!curr->fds_initialized) return NULL;
	for (struct list_elem *e = list_begin (&curr->file_descriptors); e != list_end (&curr->file_descriptors); e = list_next (e)) {
		struct file_descriptor *desc = list_entry (e, struct file_descriptor, elem);
		if (desc->fd == fd) return desc;
	}
	return NULL;
}

static int
allocate_fd (struct file *file) {
	struct thread *curr = thread_current ();
	if (!curr->fds_initialized) return -1;

	struct file_descriptor *desc = malloc (sizeof *desc);
	if (desc == NULL) return -1;
	desc->fd = curr->next_fd++;
	desc->file = file;
	desc->fd_kind = FD_FILE;
	list_push_back (&curr->file_descriptors, &desc->elem);
	return desc->fd;
}

static void
close_fd (struct file_descriptor *desc) {
	list_remove (&desc->elem);
	if (desc->fd_kind == FD_FILE && desc->file != NULL && --desc->file->ref_cnt == 0) {
		lock_acquire (&filesys_lock);
		file_close (desc->file);
		lock_release (&filesys_lock);
	}
	else if(desc->fd_kind == FD_STDIN && !desc->file) thread_current()->stdin_cnt--;
	else if(desc->fd_kind == FD_STDOUT && !desc->file) thread_current()->stdout_cnt--;
	free (desc);
}

static void
close_all_files (struct thread *t) {
	if (t == NULL || !t->fds_initialized) return;
	while (!list_empty (&t->file_descriptors)) {
		struct file_descriptor *desc = list_entry (list_begin (&t->file_descriptors), struct file_descriptor, elem);
		close_fd (desc);
	}
	t->next_fd = 0;
}

void
syscall_process_cleanup (void) {
	close_all_files (thread_current ());
}

void
update_fduplicated(struct thread *thread, bool b_value) {
	for (struct list_elem *e = list_begin (&thread->file_descriptors); e != list_end (&thread->file_descriptors); e = list_next (e)) {
		struct file_descriptor *desc = list_entry (e, struct file_descriptor, elem);
		if(desc->file) desc->file->is_duplicated = b_value;
	}
}

bool
syscall_duplicate_fds (struct thread *parent, struct thread *child) {
	ASSERT (parent != NULL);
	ASSERT (child != NULL);

	if (!parent->fds_initialized)
		return true;
	if (!child->fds_initialized) {
		list_init (&child->file_descriptors);
		// child->next_fd = 2;
		child->fds_initialized = true;
	}
	else close_all_files(child);

	update_fduplicated(parent, false);
	for (struct list_elem *e = list_begin (&parent->file_descriptors); e != list_end (&parent->file_descriptors); e = list_next (e)) {
		/* edward: allocate child file descriptor */
		struct file_descriptor *parent_fd = list_entry (e, struct file_descriptor, elem);
		struct file_descriptor *child_fd = malloc (sizeof *child_fd);
		if (child_fd == NULL) {
			close_all_files (child);
			return false;
		}

		/* edward
		CASE 00: not a file (stdin/stdout)
		CASE 01: not duplicated > duplicate a file from parent
		CASE 02: already duplicated > find duplicated file with inode address
		*/
		struct file *duplicated_file = NULL;
		if(!parent_fd->file) duplicated_file = parent_fd->file;
		else if (!parent_fd->file->is_duplicated) {
			lock_acquire (&filesys_lock);
			duplicated_file = file_duplicate (parent_fd->file);
			lock_release (&filesys_lock);
			if (duplicated_file == NULL) {
				free (child_fd);
				close_all_files (child);
				return false;
			}
			parent_fd->file->is_duplicated = true;
		}
		else {
			struct inode *identifier = parent_fd->file->inode;
			for (struct list_elem *t = list_begin (&child->file_descriptors); t != list_end (&child->file_descriptors); t = list_next (t)) {
				struct file_descriptor *desc = list_entry (t, struct file_descriptor, elem);
				if (!desc->file) continue;
				if (desc->file->inode == identifier) {
					duplicated_file = desc->file;
					if (duplicated_file) duplicated_file->ref_cnt++;
					break;
				}
			}
			if (duplicated_file == NULL) {
				lock_acquire (&filesys_lock);
				duplicated_file = file_duplicate (parent_fd->file);
				lock_release (&filesys_lock);
				if (duplicated_file == NULL) {
					free (child_fd);
					close_all_files (child);
					return false;
				}
			}
		}

		child_fd->file = duplicated_file; /* edward: link file to the descriptor */
		child_fd->fd = parent_fd->fd; /* edward: copy the descriptor number */
		child_fd->fd_kind = parent_fd->fd_kind;
		list_push_back (&child->file_descriptors, &child_fd->elem); /* edward: enlist the descriptor */
	}
	child->next_fd = parent->next_fd;
	return true;
}

int
dup2_handler (int oldfd, int newfd) {
	struct file_descriptor *desc_old = fd_lookup(oldfd);
	if(!desc_old) return -1;
	if(oldfd == newfd) return newfd;
	
	struct file_descriptor *desc_new = fd_lookup(newfd);
	if(desc_new) close_fd(desc_new);
	desc_new = malloc(sizeof(*desc_new));
	if(!desc_new) return -1;
	desc_new->fd = newfd;
	desc_new->fd_kind = desc_old->fd_kind;
	desc_new->file = desc_old->file;
	if (desc_new->fd_kind == FD_FILE && desc_new->file) desc_old->file->ref_cnt++;
	else if(desc_new->fd_kind == FD_STDIN && !desc_new->file) thread_current()->stdin_cnt++;
	else if(desc_new->fd_kind == FD_STDOUT && !desc_new->file) thread_current()->stdout_cnt++;
	list_push_back (&thread_current ()->file_descriptors, &desc_new->elem);
	return newfd;
}
