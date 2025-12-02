/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"
// #include "hash.h"
#include "kernel/hash.h"
#include "threads/mmu.h"
#include "threads/synch.h"

static struct list frame_list;  /* list for managing frame */
static struct lock frame_lock;  /* lock for frame list*/

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void vm_init(void) {
  vm_anon_init();
  vm_file_init();
#ifdef EFILESYS /* For project 4 */
  pagecache_init();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_list);
	lock_init(&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type page_get_type(struct page *page) {
  int ty = VM_TYPE(page->operations->type);
  switch (ty) {
  case VM_UNINIT:
    return VM_TYPE(page->uninit.type);
  default:
    return ty;
  }
}

/* Helpers */
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
                                    bool writable, vm_initializer *init,
                                    void *aux) {

  ASSERT(VM_TYPE(type) != VM_UNINIT);

  struct supplemental_page_table *spt = &thread_current()->spt;

  /* Check wheter the upage is already occupied or not. */
  if (spt_find_page(spt, upage) == NULL) {
    /* TODO: Create the page, fetch the initialier according to the VM type,
     * TODO: and then create "uninit" page struct by calling uninit_new. You
     * TODO: should modify the field after calling the uninit_new. */

    /* TODO: Insert the page into the spt. */
  }
err:
  return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt , void *va) 
{
	struct page p;
	p.va = pg_round_down(va);
	
  struct hash_elem *hash_elem = hash_find(&spt->h_table, &p.hash_elem);
	
  if (!hash_elem){
		return NULL;
	}
	return hash_entry(hash_elem, struct page, hash_elem);
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt, struct page *page) {
	int succ = false;
	
  struct hash_elem *old_page = hash_insert(&spt->h_table, &page->hash_elem);
	
  if (!old_page) {
		succ = true; 
	}
	return succ;
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page) {
  vm_dealloc_page(page);
  return true; /* edward: void??? true??? */
}

/* Get the struct frame, that will be evicted. */
static struct frame *vm_get_victim(void) {
  struct frame *victim = NULL;
  /* TODO: The policy for eviction is up to you. */

  return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *vm_evict_frame(void) {
  struct frame *victim UNUSED = vm_get_victim();
  /* TODO: swap out the victim and return the evicted frame. */

  return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
  struct frame *frame = malloc(sizeof(struct frame));			
	ASSERT (frame != NULL);

	void *kva = palloc_get_page(PAL_USER);
	if (kva == NULL){
		free(frame);
		PANIC("TODO: swap_out");
	}

	frame->kva = kva;
	frame->page = NULL;

	lock_acquire(&frame_lock);
	list_push_back(&frame_list, &frame->frame_elem);
	lock_release(&frame_lock);

	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void vm_stack_growth(void *addr UNUSED) {}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page) {
  destroy(page);
  free(page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va) {
	struct page *page = spt_find_page(&thread_current()->spt, va);
  
  /* No supplemental page found for this VA — cannot claim. */
  if (!page) {
    return false;
  }
  
  return vm_do_claim_page(page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	frame->page = page;
	page->frame = frame;
  
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable)) {
		page->frame = NULL;
		frame->page = NULL;
		return false;
	}
	return swap_in (page, frame->kva); /* swap the data into the frame that we got previously */
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt) {
	bool success = hash_init(&spt->h_table, __hash, __less, NULL);
	ASSERT(success); /* ??: not sure if this assertion is required */
}
/* ------------------------------------------------------------------ */
/* 					Hash Table Helper Functions */
/* ------------------------------------------------------------------ */
static uint64_t __hash(const struct hash_elem *e, void *aux) {
  const struct page *p = hash_entry(e, struct page, hash_elem);
  return hash_bytes(&p->va, sizeof(p->va));
}

/* ------------------------------------------------------------------ */
/* 					Hash Table Helper Functions */
/* ------------------------------------------------------------------ */
/* edward: compare the key */
static bool __less(const struct hash_elem *a, const struct hash_elem *b,
                   void *aux) {
  const struct page *page_a = hash_entry(a, struct page, hash_elem);
  const struct page *page_b = hash_entry(b, struct page, hash_elem);
  return (page_a->va < page_b->va);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt) {
  bool success = hash_init(&spt->h_table, __hash, __less, NULL); /* ?? */
  ASSERT(success);
}

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED,
                                  struct supplemental_page_table *src UNUSED) {}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED) {
  /* TODO: Destroy all the supplemental_page_table hold by thread and
   * TODO: writeback all the modified contents to the storage. */
}
