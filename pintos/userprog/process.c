#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "intrinsic.h"

#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *aux);
static void __do_fork (void *);

static struct sync_to_parent *wait_status_create (void);
static void wait_status_release (struct sync_to_parent *sync2p);
static void add_child_wait_status (struct thread *parent, struct sync_to_parent *sync2p);
static struct sync_to_parent *remove_child_wait_status (struct thread *parent, tid_t child_tid);
static void release_child_waits (struct thread *t);

struct initd_args {
	char *file_name;
	struct sync_to_parent *sync2p;
};

void
init_fds (struct thread *target) {
	if (!target->fds_initialized) {
		list_init(&target->file_descriptors);
		target->next_fd = 2;
		target->fds_initialized = true;

		struct file_descriptor *fd_stdin = malloc(sizeof(*fd_stdin));
		fd_stdin->fd = 0;
		fd_stdin->file = NULL;
		fd_stdin->fd_kind = FD_STDIN;
		list_push_back(&target->file_descriptors, &fd_stdin->elem);
		
		struct file_descriptor *fd_stdout = malloc(sizeof(*fd_stdout));
		fd_stdout->fd = 1;
		fd_stdout->file = NULL;
		fd_stdout->fd_kind = FD_STDOUT;
		list_push_back(&target->file_descriptors, &fd_stdout->elem);
	}
}

/*
General process initializer for initd and other process.
edward: initialize struct of current thread
*/
static void
process_init (void) {
	struct thread *current = thread_current ();
	current->stdin_cnt = 1;
	current->stdout_cnt = 1;

#ifdef USERPROG
	init_fds(current);
	if (!current->children_initialized) {
		list_init(&current->children);
		current->children_initialized = true;
	}
#endif
}

static struct sync_to_parent *
wait_status_create (void) {
	struct sync_to_parent *sync2p = malloc (sizeof *sync2p);
	if (sync2p == NULL) return NULL;
	sema_init (&sync2p->sema, 0);
	lock_init (&sync2p->lock);
	sync2p->child_tid = TID_ERROR;
	sync2p->exit_code = -1;
	sync2p->ref_cnt = 2;
	sync2p->exited = false;
	return sync2p;
}

/* edward: decrease reference by 1 and free sync2p when ref_cnt hits 0 */
static void
wait_status_release (struct sync_to_parent *sync2p) {
	bool free_ws = false;
	lock_acquire (&sync2p->lock);
	ASSERT (sync2p->ref_cnt > 0);
	sync2p->ref_cnt--;
	if (sync2p->ref_cnt == 0) free_ws = true;
	lock_release (&sync2p->lock);
	if (free_ws) free (sync2p);
}

/* edward: put sync2p into list(parent's children list) */
static void
add_child_wait_status (struct thread *parent, struct sync_to_parent *sync2p) {
	if (!parent->children_initialized) {
		list_init (&parent->children);
		parent->children_initialized = true;
	}
	list_push_back (&parent->children, &sync2p->elem);
}

/* remove the given child from the list */
static struct sync_to_parent *
remove_child_wait_status (struct thread *parent, tid_t child_tid) {
	if (!parent->children_initialized) return NULL;
	for (struct list_elem *e = list_begin (&parent->children); e != list_end (&parent->children); e = list_next (e)) {
		struct sync_to_parent *sync2p = list_entry (e, struct sync_to_parent, elem);
		if (sync2p->child_tid == child_tid) {
			list_remove (e);
			return sync2p;
		}
	}
	return NULL;
}

/* edward: delist every child left on the list and decrease the following ref_cnt of the sync_to_parent struct object */
static void
release_child_waits (struct thread *t) {
	if (!t->children_initialized) return;
	while (!list_empty (&t->children)) {
		struct sync_to_parent *sync2p = list_entry (list_pop_front (&t->children), struct sync_to_parent, elem);
		wait_status_release (sync2p);
	}
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	process_init ();
	struct initd_args *args = malloc (sizeof *args);
	if (args == NULL) return TID_ERROR;
	args->sync2p = wait_status_create ();
	if (args->sync2p == NULL) {
		free (args);
		return TID_ERROR;
	}

	char *fn_copy = palloc_get_page (0); /* Make a copy of FILE_NAME. Otherwise there's a race between the caller and load(). */
	if (fn_copy == NULL) {
		wait_status_release (args->sync2p);
		wait_status_release (args->sync2p);
		free (args);
		return TID_ERROR;
	}
	strlcpy (fn_copy, file_name, PGSIZE);
	args->file_name = fn_copy;

	char thread_name[16];
	strlcpy (thread_name, file_name, sizeof thread_name);
	char *space = strchr (thread_name, ' ');
	if (space != NULL) *space = '\0';

	enum intr_level old_level = intr_disable ();
	tid_t tid = thread_create (thread_name, PRI_DEFAULT, initd, args); /* Create a new thread to execute FILE_NAME. */
	if (tid == TID_ERROR) {
		intr_set_level (old_level);
		palloc_free_page (fn_copy);
		wait_status_release (args->sync2p);
		wait_status_release (args->sync2p);
		free (args);
		return TID_ERROR;
	}
	args->sync2p->child_tid = tid;
	add_child_wait_status (thread_current (), args->sync2p);
	intr_set_level (old_level);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *aux) {
	struct initd_args *args = aux;
	struct sync_to_parent *sync2p = args->sync2p;
	char *file_name = args->file_name;
	free (args);
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	thread_current ()->sync2p = sync2p;
	process_init ();

	if (process_exec (file_name) < 0) PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/*
Clones the current process as `name`.
Returns the new process's thread id, or TID_ERROR if the thread cannot be created.
*/
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	process_init ();
	
	/* edward: make fork structure */
	struct fork_struct *fs = malloc(sizeof *fs);
	if(!fs) return TID_ERROR;
	fs->sync2p = wait_status_create ();
	if (fs->sync2p == NULL) {
		free (fs);
		return TID_ERROR;
	}
	
	/* edward: set up current thread */
	fs->parent = thread_current();
	memcpy(&fs->parent_if, if_, sizeof fs->parent_if);
	sema_init(&fs->semaphore, 0);
	fs->success = false;

	/* edward: fork */
	enum intr_level old_level = intr_disable (); /* edward: protection for fork struct */
	tid_t tid = thread_create(name, PRI_DEFAULT, __do_fork, fs);

	if(tid == TID_ERROR) { /* edward: in case of creation failure */
		intr_set_level (old_level);
		/* edward: parent + child */
		wait_status_release (fs->sync2p);
		wait_status_release (fs->sync2p);
		free(fs);
		return TID_ERROR;
	}

	/* edward: enlist the child to the list of parent thread struct */
	fs->sync2p->child_tid = tid;
	add_child_wait_status (thread_current (), fs->sync2p);
	intr_set_level (old_level);

	sema_down(&fs->semaphore); /* edward: wait for child to wake it up after the fork */
	if(fs->success) {
		free(fs);
		return tid;
	}
	remove_child_wait_status (thread_current (), tid);
	wait_status_release (fs->sync2p);
	free(fs);
	return TID_ERROR;
}

#ifndef VM
/*
Duplicate the parent's address space by passing this function to the pml4_for_each.
This is only for the project 2.
*/
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	/* edward
	pte: parent's page table entry.
	va: page address where parent's pte points at.
	aux: currently thread struct pointer of parent thread.
	*/
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* edward
	how is that even possible?
	does user program have any kernel page in their page table?????????
	"pml4_create" method copies the "base_pml4"(kernel mappings) right into the user process' page table
	*/
	if(is_kernel_vaddr(va)) return true; /* 1. TODO: If the parent_page is kernel page, then return immediately. */
	parent_page = pml4_get_page (parent->pml4, va); /* 2. Resolve VA from the parent's page map level 4. */
	newpage = palloc_get_page(PAL_USER); /* 3. TODO: Allocate new PAL_USER page for the child and set result to NEWPAGE. */
	if(!newpage) return false;
	memcpy(newpage, parent_page, PGSIZE); /* 4. TODO: Duplicate parent's page to the new page and check whether parent's page is writable or not (set WRITABLE according to the result). */
	writable = is_writable(pte);
	if (!pml4_set_page (current->pml4, va, newpage, writable)) { /* 5. Add new page to child's page table at address VA with WRITABLE permission. */
		palloc_free_page(newpage); /* 6. TODO: if fail to insert page, do error handling. */
		return false;
	}
	return true;
}
#endif

/*
A thread function that copies parent's execution context.
Hint)
parent->tf does not hold the userland context of the process. 🔥
That is, you are required to pass second argument of process_fork to this function.
*/
static void
__do_fork (void *aux) {
	struct fork_struct *fs = aux;
	struct intr_frame if_;
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) fs->parent;
	current->sync2p = fs->sync2p;
	struct intr_frame *parent_if = &fs->parent_if; /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL) goto error;
	process_activate (current);

	current->running_file = file_duplicate(parent->running_file);
	if(!current->running_file){
		goto error;	
	}
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt)) goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent)) goto error; /* edward: copy page table */
#endif

	/* 3. struct thread */
	process_init ();
	if (!syscall_duplicate_fds (parent, current)) goto error;
	current->stdin_cnt = parent->stdin_cnt;
	current->stdout_cnt = parent->stdout_cnt;
	fs->success = true;
	sema_up(&fs->semaphore); /* edward: wake parent up */
	do_iret (&if_); /* Finally, switch to the newly created process. */
error:
	fs->success = false;
	sema_up(&fs->semaphore);
	thread_exit ();
}

/*
Switch the current execution context to the f_name.
Returns -1 on fail.
*/
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	process_cleanup (); /* We first kill the current context */
#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif
	success = load (file_name, &_if); /* And then load the binary */
	/* test 46 fails.. */
	palloc_free_page (file_name);
	if (!success) {
		thread_current ()->exit_status = -1;
		thread_exit (); /* If load failed, terminate the process. */
	}
	do_iret (&_if); /* Start switched process. */
	NOT_REACHED ();
}


/*
Waits for thread TID to die and returns its exit status.
If it was terminated by the kernel (i.e. killed due to an exception), returns -1.
If TID is invalid or if it was not a child of the calling process,
or if process_wait() has already been successfully called for the given TID, returns -1 immediately, without waiting.

This function will be implemented in problem 2-2.
For now, it does nothing.
*/
int
process_wait (tid_t child_tid) {
	struct sync_to_parent *sync2p = remove_child_wait_status (thread_current (), child_tid);
	if (sync2p == NULL) return -1;
	sema_down (&sync2p->sema);
	lock_acquire (&sync2p->lock);
	int status = sync2p->exit_code;
	lock_release (&sync2p->lock);
	wait_status_release (sync2p);
	return status;
}

/*
Exit the process.
This function is called by thread_exit ().
*/
void
process_exit (void) {
	/*
	uint64_t *pml4;
	struct list file_descriptors;
	int next_fd;
	bool fds_initialized;
	struct list children;
	bool children_initialized;
	struct sync_to_parent *sync2p;
	*/
	struct thread *curr = thread_current ();
	if (curr->pml4 != NULL) printf ("%s: exit(%d)\n", curr->name, curr->exit_status);

	release_child_waits (curr);
	if (curr->sync2p != NULL) {
		lock_acquire (&curr->sync2p->lock);
		curr->sync2p->exit_code = curr->exit_status;
		curr->sync2p->exited = true; /* edward: alert parent that current child process finished */
		lock_release (&curr->sync2p->lock);
		sema_up (&curr->sync2p->sema); /* edward: wake parent up */
		wait_status_release (curr->sync2p);
		curr->sync2p = NULL;
	}
	if (curr->running_file) {
		// file_allow_write(curr->running_file);
		file_close(curr->running_file);
		curr->running_file = NULL;
	}
	syscall_process_cleanup();
	process_cleanup ();
}

/*
Free the current process's resources.
edward: destroy page table
*/
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

	if (curr->running_file != NULL) {
        file_close(curr->running_file);
        curr->running_file = NULL;
    }
#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	char *file_name_copy = NULL;
	enum { MAX_ARGS = LOADER_ARGS_LEN / 2 + 1 };
	char *argv[MAX_ARGS];
	uintptr_t argv_addrs[MAX_ARGS];
	int argc = 0;
	char *token, *save_ptr;

	file_name_copy = palloc_get_page (PAL_ZERO);
	if (file_name_copy == NULL) goto done;
	strlcpy (file_name_copy, file_name, PGSIZE);

	/* edward: parse the token */
	for (token = strtok_r (file_name_copy, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr)) {
		if (argc >= MAX_ARGS) goto done;
		argv[argc++] = token;
	}
	if (argc == 0) goto done;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* edward: open requested ELF file. */
	file = filesys_open (argv[0]); /* test 46 fails.. */
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff; /* edward: ELF Program Header Offset */
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) /* edward: read program header from ELF file */
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page, read_bytes, zero_bytes, writable)) goto done;
				}
				else goto done;
				break;
		}
	}

	if (!setup_stack (if_)) goto done; /* edward: set up the stack for the process. */
	if_->rip = ehdr.e_entry; /* edward: put the instruction pointer. */

	/* edward: pushing arguments */
	for (i = argc - 1; i >= 0; i--) {
		size_t arg_len = strlen (argv[i]) + 1;
		if_->rsp -= arg_len;
		memcpy ((void *) if_->rsp, argv[i], arg_len);
		argv_addrs[i] = if_->rsp;
	}

	/* edward: pushing padding for the 16 bytes alignment. */
	size_t padding = if_->rsp % 16;
	if (padding) {
		if_->rsp -= padding;
		memset ((void *) if_->rsp, 0, padding);
	}

	/* edward: pushing null sentinel */
	if_->rsp -= sizeof (uintptr_t);
	memset ((void *) if_->rsp, 0, sizeof (uintptr_t));

	/* edward: pushing addresses of arguments */
	for (i = argc - 1; i >= 0; i--) {
		if_->rsp -= sizeof (uintptr_t);
		memcpy ((void *) if_->rsp, &argv_addrs[i], sizeof (uintptr_t));
	}
	
	/* edward: not sure if these further information pushes are required */
	uintptr_t argv_start = if_->rsp;
	
	if_->rsp -= sizeof (uintptr_t);
	memcpy ((void *) if_->rsp, &argv_start, sizeof (uintptr_t));

	if_->rsp -= sizeof (uintptr_t);
	memcpy ((void *) if_->rsp, &argc, sizeof (uintptr_t));

	if_->rsp -= sizeof (uintptr_t);
	memset ((void *) if_->rsp, 0, sizeof (uintptr_t));

	if_->R.rdi = argc;
	if_->R.rsi = argv_start;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	if (!success) file_close (file);
	else thread_current()->running_file = file;
	if (file_name_copy != NULL) palloc_free_page (file_name_copy);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	struct lazy_load_aux *load_aux = aux;
	/* TODO: This called when the first page fault occurs on address VA. */
	void *kva = page->frame->kva;

	if (file_read_at (load_aux ->file, kva, load_aux ->read_bytes, load_aux ->ofs) != (int) load_aux ->read_bytes) {
		free (load_aux);
		return false;
	}

	memset (kva + load_aux ->read_bytes, 0, load_aux->zero_bytes);
	
	free (aux);

  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable) {

	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	enum vm_type vm_type = VM_UNINIT;
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes; // 0으로 채울 공간 있는지

		struct lazy_load_aux *aux = malloc(sizeof(struct lazy_load_aux));
		aux->file = file;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->ofs = ofs;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		if (!vm_alloc_page_with_initializer (VM_ANON, upage, writable, lazy_load_segment, aux)) {
			free(aux);
			return false;
		}
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;

		ofs += page_read_bytes;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	if (vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_bottom, true, NULL, NULL)){
		if (vm_claim_page(stack_bottom)) {
			success = true;
		} else {
      vm_dealloc_page(spt_find_page(&thread_current()->spt, stack_bottom));
    }
	}

	if (success) {
		if_->rsp = USER_STACK;
	}
	return success;
}
#endif /* VM */