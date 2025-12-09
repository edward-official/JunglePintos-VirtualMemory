/* vm.c: Generic interface for virtual memory objects. */

#include "vm/vm.h"
#include "kernel/hash.h"
#include "threads/mmu.h"
#include "threads/synch.h"
#include "vm/inspect.h"
#include "threads/malloc.h"
#include "include/userprog/process.h"
#include "lib/string.h"

#define STACK_LIMIT (1 << 20)

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
static void spt_destructor(struct hash_elem *e, void *aux);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable, vm_initializer *init, void *aux) {
  ASSERT(VM_TYPE(type) != VM_UNINIT);

  struct supplemental_page_table *spt = &thread_current()->spt;
  struct page *page = NULL;
  if (spt_find_page(spt, upage) == NULL) {
    page = malloc(sizeof(struct page));
    if (!page) {
      goto err;
    }

    bool (*initializer)(struct page *, enum vm_type, void *);
    switch (VM_TYPE(type)) {
      case VM_ANON:
        initializer = anon_initializer;
        break;
      case VM_FILE:
        initializer = file_backed_initializer;
        break;
      default:
        goto err;
    }
    
    uninit_new(page, pg_round_down(upage), init, type, aux, initializer);
    
    page->writable = writable;

    if (!spt_insert_page(spt, page)) {
      goto err;
    }
  }

  return true;
err:
  if (page) {
    free(page);
  }
  return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt , void *va) {
  struct page p;
  p.va = pg_round_down(va);
  
  struct hash_elem *hash_elem = hash_find(&spt->h_table, &p.hash_elem);
  
  if (!hash_elem) {
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
  if (!kva) {
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
static void vm_stack_growth(void *addr) {
  void *stack_bottom = pg_round_down(addr);

  if(!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0, stack_bottom, true, NULL, NULL)){
    return;
  }
}

/* Handle the fault on write_protected page */
static bool vm_handle_wp(struct page *page UNUSED) {}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr, bool user, bool write, bool not_present) {
  /* TODO: Validate the fault */
  if (!not_present) {
    return false;
  }
  if (!addr || !is_user_vaddr(addr) || pg_round_down(addr) < (void *) PGSIZE) {
    return false;
  }

  struct supplemental_page_table *spt = &thread_current()->spt;
  struct page *page = spt_find_page(spt, pg_round_down(addr));
  
  /* TODO: Your code goes here */
  if (!page) {
    void *rsp = user ? f->rsp : thread_current()->user_rsp;
    if (addr <= USER_STACK &&  addr >= USER_STACK - (1 << 20) && rsp - 8 <= addr) {
        
      vm_stack_growth(addr);
       
      page = spt_find_page(spt, pg_round_down(addr));
      if (!page) {
        return false;
      }
    }
  }

  if (write && !page->writable) {
    return false;
  }

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

/* Copy supplemental page table from src to dst */
bool supplemental_page_table_copy(struct supplemental_page_table *dst , struct supplemental_page_table *src) {
  ASSERT(dst && src);
  ASSERT(dst == &thread_current()->spt);
  
  bool success = false;
  struct hash_iterator iter;
  
  hash_first (&iter, &src->h_table);
  struct hash_elem *elem;
  while (elem = hash_next(&iter)) {
    /*get page*/
    struct page *src_page = hash_entry(elem, struct page, hash_elem);
    if (!src_page) {
      return false;
    }
    
    /*VM_TYPE to switch*/
    switch (VM_TYPE(src_page->operations->type)) {
        case VM_UNINIT:
          success = __copy_uninit(src_page);
          break;

        default:
          success = __copy_init(src_page);
          break;
    }
    if (!success){
      return false;
    }
  }
  return true;
}


/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt) {
  hash_destroy(&spt->h_table, __destructor); 
}



//// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE //// HELPER STARTS HERE ////
static uint64_t __hash(const struct hash_elem *e, void *aux) {
  const struct page *p = hash_entry(e, struct page, hash_elem);
  return hash_bytes(&p->va, sizeof(p->va));
}

/* edward: compare the key */
static bool __less(const struct hash_elem *a, const struct hash_elem *b, void *aux) {
  const struct page *page_a = hash_entry(a, struct page, hash_elem);
  const struct page *page_b = hash_entry(b, struct page, hash_elem);
  return (page_a->va < page_b->va);
}

static void __destructor (struct hash_elem *e, void *aux) {
    struct page *page = hash_entry(e, struct page, hash_elem);
    
      vm_dealloc_page(page);
}

static bool __copy_uninit(struct page *src_page) {
  ASSERT(src_page->operations->type == VM_UNINIT);
  
  enum vm_type intended_type = page_get_type(src_page);
  void *va = src_page->va;
  bool writable = src_page->writable;

  struct uninit_page *src_uninit = &src_page->uninit;
  struct lazy_load_aux *aux_copy = NULL;

  if (src_uninit->aux) {
    aux_copy = malloc(sizeof(struct lazy_load_aux));
    if (!aux_copy) {
      return false;
    }
    memcpy(aux_copy, src_uninit->aux, sizeof(struct lazy_load_aux));
  }

  if (intended_type == VM_ANON){
    aux_copy->file = thread_current()->running_file;
  }

  if (!vm_alloc_page_with_initializer(intended_type, va, writable, src_uninit->init, aux_copy)) {
    if (aux_copy) {
      free(aux_copy);
    }
    return false;
  }

  return true;
}

static bool __copy_init(struct page *src_page) {
  ASSERT(src_page->operations->type != VM_UNINIT);

  enum vm_type intended_type = page_get_type(src_page);
  void *va = src_page->va;
  bool writable = src_page->writable;

  if (!vm_alloc_page_with_initializer(intended_type, va, writable, NULL, NULL)) {
    return false;
  }
  
  // TODO: further implement be needed (swap case)
  if (!vm_claim_page(va)) {
    return false;
  }

  struct page *target_page = spt_find_page(&thread_current()->spt, va);
  if (!target_page || !target_page->frame) {
    return false;
  }
  memcpy(target_page->frame->kva, src_page->frame->kva, PGSIZE);

  return true;
}

//// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE //// HELPER STOPS HERE ////
