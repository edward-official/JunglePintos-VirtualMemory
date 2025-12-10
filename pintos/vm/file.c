/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include <debug.h>
#include <string.h>
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);
static bool lazy_load_file (struct page *page, void *aux);

static void release_mmap_info (struct mmap_info *info);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;
	page->file.mmap_info = NULL;
	page->file.file = NULL;
	page->file.offset = 0;
	page->file.read_bytes = 0;
	page->file.zero_bytes = 0;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page = &page->file;
	if (file_page->mmap_info == NULL || file_page->file == NULL) {
		return false;
	}

	lock_acquire (&filesys_lock);
	int read = file_read_at (file_page->file, kva, file_page->read_bytes, file_page->offset);
	lock_release (&filesys_lock);
	if (read != (int) file_page->read_bytes) {
		return false;
	}
	memset (kva + file_page->read_bytes, 0, file_page->zero_bytes);
	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page = &page->file;
	if (page->frame == NULL || file_page->mmap_info == NULL) {
		return true;
	}

	struct thread *curr = thread_current ();
	if (pml4_is_dirty (curr->pml4, page->va)) {
		lock_acquire (&filesys_lock);
		file_write_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset);
		lock_release (&filesys_lock);
		pml4_set_dirty (curr->pml4, page->va, false);
	}
	pml4_clear_page (curr->pml4, page->va);
	page->frame->page = NULL;
	page->frame = NULL;
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page = &page->file;
	struct mmap_info *info = file_page->mmap_info;

	if (page->frame && pml4_is_dirty (thread_current ()->pml4, page->va)) {
		lock_acquire (&filesys_lock);
		file_write_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset);
		lock_release (&filesys_lock);
	}
	if (page->frame) {
		pml4_clear_page (thread_current ()->pml4, page->va);
		page->frame->page = NULL;
		page->frame = NULL;
	}

	if (info) {
		release_mmap_info (info);
	}
}

void
file_lazy_aux_release(struct lazy_load_aux *aux) {
	if (aux == NULL) return;
	struct mmap_info *info = aux->mmap_info;
	if (info) release_mmap_info (info);
	free (aux);
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable, struct file *file, off_t offset) {
	lock_acquire (&filesys_lock);
	struct file *reopened = file_reopen (file);
	lock_release (&filesys_lock);
	if (reopened == NULL) return NULL;

	lock_acquire (&filesys_lock);
	off_t file_len = file_length (reopened);
	lock_release (&filesys_lock);

	struct mmap_info *info = malloc (sizeof *info);
	if (info == NULL) {
		file_close (reopened);
		return NULL;
	}
	info->file = reopened;
	info->start = addr;
	info->length = length;
	info->page_cnt = 0;

	void *upage = addr;
	size_t remaining = length;
	off_t ofs = offset;

	while (remaining > 0) {
		size_t page_len = remaining < PGSIZE ? remaining : PGSIZE;
		size_t page_read_bytes = 0;
		if (ofs < file_len) {
			off_t file_left = file_len - ofs;
			if (file_left > 0) {
				page_read_bytes = file_left < (off_t)page_len ? (size_t)file_left : page_len;
			}
		}
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct lazy_load_aux *aux = malloc (sizeof *aux);
		if (aux == NULL) {
			if (info->page_cnt == 0) {
				file_close (reopened);
				free (info);
			} else {
				do_munmap (addr);
			}
			return NULL;
		}
		aux->file = reopened;
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		aux->mmap_info = info;

		if (!vm_alloc_page_with_initializer (VM_FILE, upage, writable, lazy_load_file, aux)) {
			if (info->page_cnt == 0) {
				file_close (reopened);
				free (info);
			} else {
				do_munmap (addr);
			}
			free (aux);
			return NULL;
		}

		info->page_cnt++;
		remaining -= page_len;
		upage += PGSIZE;
		ofs += page_len;
	}

	return addr;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	struct thread *curr = thread_current ();
	struct page *first = spt_find_page (&curr->spt, addr);
	if (first == NULL) return;

	struct mmap_info *info = NULL;
	if (VM_TYPE (first->operations->type) == VM_UNINIT) {
		struct lazy_load_aux *aux = first->uninit.aux;
		if (aux == NULL) return;
		info = aux->mmap_info;
	} else if (VM_TYPE (first->operations->type) == VM_FILE) {
		info = first->file.mmap_info;
	}
	if (info == NULL) return;

	void *start = info->start;
	size_t length = info->length;
	for (size_t offset = 0; offset < length; offset += PGSIZE) {
		void *va = start + offset;
		struct page *page = spt_find_page (&curr->spt, va);
		if (page == NULL) continue;
		hash_delete (&curr->spt.h_table, &page->hash_elem);
		vm_dealloc_page (page);
	}
}

static bool
lazy_load_file (struct page *page, void *aux) {
	struct lazy_load_aux *file_aux = aux;
	struct file_page *file_page = &page->file;

	file_page->mmap_info = file_aux->mmap_info;
	file_page->file = file_aux->file;
	file_page->offset = file_aux->ofs;
	file_page->read_bytes = file_aux->read_bytes;
	file_page->zero_bytes = file_aux->zero_bytes;

	lock_acquire (&filesys_lock);
	int read = file_read_at (file_page->file, page->frame->kva, file_page->read_bytes, file_page->offset);
	lock_release (&filesys_lock);

	if (read != (int) file_page->read_bytes) {
		free (file_aux);
		return false;
	}

	memset (page->frame->kva + file_page->read_bytes, 0, file_page->zero_bytes);
	free (file_aux);
	return true;
}

static void
release_mmap_info (struct mmap_info *info) {
	if (info == NULL) return;
	ASSERT (info->page_cnt > 0);
	info->page_cnt--;
	if (info->page_cnt == 0) {
		lock_acquire (&filesys_lock);
		file_close (info->file);
		lock_release (&filesys_lock);
		free (info);
	}
}
