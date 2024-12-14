#include "types.h"
#include "param.h"
#include "layout.h"
#include "riscv.h"
#include "defs.h"
#include "buf.h"
#include "elf.h"

#include <stdbool.h>

struct elfhdr* kernel_elfhdr;
struct proghdr* kernel_phdr;

uint64 find_kernel_load_addr(enum kernel ktype) {
    /* CSE 536: Get kernel load address from headers */
    kernel_elfhdr = (ktype == NORMAL)? (struct elfhdr *)RAMDISK // Initialize ELF header pointer to 0x84000000
	    : (struct elfhdr *) RECOVERYDISK;			// Initialize ELF header pointer to 0x84500000
    
    // Check if ELF magic bytes are correct
    if (kernel_elfhdr->magic != ELF_MAGIC) {
        return 0;
    }

    // Calculate program header offset and size 
    uint64 phoff = kernel_elfhdr->phoff;
    ushort phentsize = kernel_elfhdr->phentsize;
    kernel_phdr = (struct proghdr*)(RAMDISK + phoff + phentsize);
    
    // kernload-start, virtual address of .text section
    return kernel_phdr->vaddr;
}

uint64 find_kernel_size(enum kernel ktype) {
    /* CSE 536: Get kernel binary size from headers */
    kernel_elfhdr = (ktype == NORMAL)? (struct elfhdr *)RAMDISK // Initialize ELF header pointer to 0x84000000
	    : (struct elfhdr *) RECOVERYDISK;			// Initialize ELF header pointer to 0x84500000
    
    // Check if ELF magic bytes are correct
    if (kernel_elfhdr->magic != ELF_MAGIC) {
        return 0;
    }

    // Size = start of section header + (size of section header * number of section headers)
    // eg. for kernel1, size = 276936 + (18 * 64) = 278088 bytes = size in ls -al kernel1
    uint64 shoff = kernel_elfhdr->shoff;
    ushort shentsize = kernel_elfhdr->shentsize;
    ushort shnum = kernel_elfhdr->shnum;
    return (uint64)(shoff + shentsize*shnum);
}

uint64 find_kernel_entry_addr(enum kernel ktype) {
    /* CSE 536: Get kernel entry point from headers */ 
    kernel_elfhdr = (ktype == NORMAL)? (struct elfhdr *)RAMDISK // Initialize ELF header pointer to 0x84000000
	    : (struct elfhdr *) RECOVERYDISK;			// Initialize ELF header pointer to 0x84500000
    
    // Check if ELF magic bytes are correct
    if (kernel_elfhdr->magic != ELF_MAGIC) {
        return 0;
    }
    
    // eg. for kernel1, entry addr = 0x81000000
    return kernel_elfhdr->entry;
}
