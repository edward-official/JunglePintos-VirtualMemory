#ifndef VM_FILE_H
#define VM_FILE_H
#include <stddef.h>
#include "filesys/file.h"

struct mmap_info;
struct lazy_load_aux;
struct page;
enum vm_type;

struct mmap_info {
	struct file *file;      /* Reopened file for this mapping. */
	void *start;            /* Mapping start address. */
	size_t length;          /* Mapping length in bytes. */
	size_t page_cnt;        /* Pages tracked by this mapping. */
};

struct file_page {
	struct mmap_info *mmap_info;
	struct file *file;
	off_t offset;
	size_t read_bytes;
	size_t zero_bytes;
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable, struct file *file, off_t offset);
void do_munmap (void *va);
void file_lazy_aux_release(struct lazy_load_aux *aux);
#endif
