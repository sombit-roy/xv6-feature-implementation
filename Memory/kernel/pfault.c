/* This file contains code for a generic page fault handler for processes. */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

int loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz);
int flags2perm(int flags);

/* Read current time. */
uint64 read_current_timestamp() {
  uint64 curticks = 0;
  acquire(&tickslock);
  curticks = ticks;
  wakeup(&ticks);
  release(&tickslock);
  return curticks;
}

bool psa_tracker[PSASIZE];

/* All blocks are free during initialization. */
void init_psa_regions(void)
{
    for (int i = 0; i < PSASIZE; i++) 
        psa_tracker[i] = false;
}

/* Evict heap page to disk when resident pages exceed limit */
void evict_page_to_disk(struct proc* p) {
    /* Find free block */
    int blockno = 0;
    for (int block = PSASTART; block < PSAEND - 3; block += 4) {
      if (!psa_tracker[block]) {
        blockno = block;
	break;
      }
    }
    for (int i = 0; i < 4; i++) {
      psa_tracker[blockno + i] = true;
    }

    /* Find victim page using FIFO. */
    int oldest_idx = -1;
    uint64 oldest_time = 0xFFFFFFFFFFFFFFFF;
    for (int i = 0; i < MAXHEAP; i++) {
      if (p->heap_tracker[i].loaded && p->heap_tracker[i].last_load_time < oldest_time) {
        oldest_idx = i;
        oldest_time = p->heap_tracker[i].last_load_time;
      }
    }

    uint64 victim_addr = p->heap_tracker[oldest_idx].addr;

    /* Print statement. */
    print_evict_page(victim_addr, blockno - PSASTART);
    p->heap_tracker[oldest_idx].loaded = false;
    p->heap_tracker[oldest_idx].startblock = blockno;

    /* Read memory from the user to kernel memory first. */
    char *kpage = kalloc();
    copyin(p->pagetable, kpage, victim_addr, PGSIZE);    

    /* Write to the disk blocks. */
    struct buf* b;
    for (int i = 0; i < 4; i++) {
      b = bread(1, PSASTART + (blockno + i));
      // Copy page contents to b.data using memmove.
      memmove(b->data, kpage + i*BSIZE, BSIZE);
      bwrite(b);
      brelse(b);
    }
    
    /* Unmap swapped out page */
    uvmunmap(p->pagetable, victim_addr, 1, 1);

    /* Update the resident heap tracker. */
    p->heap_tracker[oldest_idx].loaded = false;
    p->heap_tracker[oldest_idx].startblock = blockno;
    p->resident_heap_pages--;
    kfree(kpage);
}

/* Retrieve faulted page from disk. */
void retrieve_page_from_disk(struct proc* p, uint64 uvaddr) {
    /* Find where the page is located in disk */
    int page_idx = -1;
    for (int i = 0; i < MAXHEAP; i++) {
      if (p->heap_tracker[i].addr == uvaddr && p->heap_tracker[i].startblock != -1) {
        page_idx = i;
	break;
      }
    }
    if (page_idx == -1) {
      panic("Page not found in PSA\n");
    }

    int startblock = p->heap_tracker[page_idx].startblock;

    /* Print statement. */
    print_retrieve_page(uvaddr, startblock - PSASTART);

    /* Create a kernel page to read memory temporarily into first. */
    char *kpage = kalloc();

    /* Read the disk block into temp kernel page. */
    for (int i = 0; i < 4; i++) {
      struct buf* b = bread(1, PSASTART + startblock + i);
      memmove(kpage + i*BSIZE, b->data, BSIZE);
      brelse(b);
    }

    /* Copy from temp kernel page to uvaddr (use copyout) */
    copyout(p->pagetable, uvaddr, kpage, PGSIZE);
    p->heap_tracker[page_idx].loaded = false;
    p->heap_tracker[page_idx].startblock = -1;
    p->resident_heap_pages++;
    kfree(kpage);
}


void page_fault_handler(void) 
{
    /* Current process struct */
    struct proc *p = myproc();

    /* Find faulting address. */
    uint64 faulting_addr = PGROUNDDOWN(r_stval());  // round down to page boundary
    print_page_fault(p->name, faulting_addr);

    /* Track whether the heap page should be brought back from disk or not. */
    bool load_from_disk = false;
    for (int i = 0; i < MAXHEAP; i++) {
      if (p->heap_tracker[i].addr == faulting_addr && p->heap_tracker[i].startblock != -1) {
        load_from_disk = true;
	break;
      }
    }

    /* Check if the fault address is a heap page. Use p->heap_tracker */
    bool is_heap_page = false;
    int heap_idx = -1;
    for (int i = 0; i < MAXHEAP; i++) {
      if (p->heap_tracker[i].addr == faulting_addr) {
        is_heap_page = true;
	heap_idx = i;
	break;
      }
    }
    if (is_heap_page) {
        goto heap_handle;
    }

    /* If it came here, it is a page from the program binary that we must load. */
    struct elfhdr elf;
    struct proghdr ph;
    // Resolve process into an inode
    struct inode* ip = namei(p->name);

    if (ip == 0) {
      panic("Process name not found\n");
    }
    
    // Read ELF header from inode to elfhdr struct
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf) || elf.magic != ELF_MAGIC) {
      panic("ELF header read failed\n");
    }

    // Iterate over program headers to find relevant segments, similar to loading into memory in exec.c
    for (int i = 0, off = elf.phoff; i < elf.phnum; i++, off+= sizeof(ph)) {
      if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) {
        panic("Program header read failed");
      }
      
      // Only loadable sections
      if (ph.type != ELF_PROG_LOAD) {
        continue;
      }
      
      if (faulting_addr >= ph.vaddr && faulting_addr < ph.vaddr + ph.memsz) {
        if (uvmalloc(p->pagetable, faulting_addr, faulting_addr + ph.memsz, flags2perm(ph.flags)) == 0) {
	  panic("Free page allocation failed\n");
	}
	
	uint64 seg_offset = faulting_addr - ph.vaddr;
	if (loadseg(p->pagetable, faulting_addr, ip, ph.off + seg_offset, PGSIZE) < 0) {
	  panic("Segment load failed\n");
	}
	
	print_load_seg(faulting_addr, ph.off + seg_offset, PGSIZE);
      }
    }

    /* Go to out, since the remainder of this code is for the heap. */
    goto out;

heap_handle:
    /* 2.4: Check if resident pages are more than heap pages. If yes, evict. */
    if (p->resident_heap_pages == MAXRESHEAP) {
        evict_page_to_disk(p);
    }

    /* 2.3: Map a heap page into the process' address space. (Hint: check growproc) */
    if ((uvmalloc(p->pagetable, faulting_addr, faulting_addr + PGSIZE, PTE_W | PTE_R)) ==  0) {
      panic("Heap page allocation failed\n");
    }

    /* 2.4: Update the last load time for the loaded heap page in p->heap_tracker. */
    p->heap_tracker[heap_idx].loaded = true;
    p->heap_tracker[heap_idx].last_load_time = read_current_timestamp();

    /* 2.4: Heap page was swapped to disk previously. We must load it from disk. */
    if (load_from_disk) {
        retrieve_page_from_disk(p, faulting_addr);
    }

    /* Track that another heap page has been brought into memory. */
    p->resident_heap_pages++;

out:
    /* Flush stale page table entries. This is important to always do. */
    sfence_vma();
    return;
}
