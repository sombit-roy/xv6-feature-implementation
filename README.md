# xv6 feature implementation

## Bootloader
The first assignment focuses on booting up the OS kernel, and implementing security measures in the boot process.

### Boot ROM and linker
The Boot ROM is a piece of code located in the CPU chip itself and is immediately available to the processor upon power-on. In our case, it is provided by QEMU to the RISC-V virtual machine. The code is loaded at `0x1000`. Upon inspecting the assembly instructions using GDB, we see that it jumps to address `0x80000000`, which is the entry point specified in the linker script.

The linker is the final part of the compilation process, which figures out the entry address of each section of the object file and combines them with the libc libraries to generate the executable. In this assignment, we specified the entry point for sections `text`, `data`, `rodata` and `bss`.

### Booting up different kernel binaries
First, we have to set up the stack (whose address is specified by `sys_info.bl_start`). Without setting up a stack, the value of the stack pointer could be uninitialized and random. The function frame will not be well-defined, so during function calls, the return address will be pushed to unintended locations causing the program to crash when returning to caller. 

`entry.S` jumps to the `start()` function in machine mode on `stack0`. The `mstatus` register's bits [12:11] (Machine Previous Privilege) controls privilege level to which CPU will return on `mret` instruction. These  bits are cleared to make MPP supervisor mode.

Before loading the kernel, 3 things must be figured out - the load address, the size and the entry address. To get these values, we have to inspect the ELF header and the program header, which contains metadata for the binary. It is given that our `struct elfhdr` is located at `RAMDISK 0x84000000`. After verifying the magic bytes, we can calculate that program header from its offset and size, and return the load (virtual) address of `.text`. The kernel size can be determined as (section offset + size of section header * number of section headers). For example, the size of `kernel1` is 276396 + 18 * 64 = 277548 bytes, which can be verified using `ls -al kernel1`. Lastly, the entry point is directly extracted from the ELF header.

`kernel_copy` in `load.c` copies the kernel binary located in the disk and loads it into a temporary buffer in memory. Since it can only move 1024 bytes at a time, we call it in a loop to copy blocks of data to the buffer. First, it performs a check whether the disk block number is greater than the total number of blocks available to prevent reading outside the block range. Next, it calculates the byte offset in the disk where the block starts. Each block has a size of 1024 bytes, and the disk address is calculated by multiplying the block number by the block size. To calculate the address from which kernel data will be copied, it adds the offset to `RAMDISK`. Then, using the `memmove` function defined in `string.c`, it copies 1024 bytes of data from the source address (disk) to the data field of buffer. Lastly, it marks the buffer as valid to indicate no errors.

The final steps are providing QEMU's DRAM start and end (given in `layout.h`) to kernel, disabling paging register `satp`, write kernel entry point to the program counter register `mepc`, delegating interrupts and exceptions to supervisor mode and executing `mret` to jump to `main()`.

### Physical memory protection
In RISC-V, PMP is a feature which prevents memory regions from being accessed by lower privilege levels. In TOR, the bits set in the `pmpaddr0` register encode the highest address available to the kernel, and the bits set in the `pmpcfg0` register encode the mode and read/write/execute permissions. In `kernelpmp1`, to isolate 117 MB, `pmpaddr0 = (0x80000000 + 0x7500000) >> 2`. Note: The start physical address provided by QEMU is the bootloader start address, and after adding 117 MB it must be right bit shifted by 2 to maintin the 4-byte granularity. Also, mode = `b'01`, R/W/X = `b'111` $\implies$ `pmpcfg0 = 0xf`.

In NAPOT, the least significant zero bit encodes the size of the memory region. For example in `kernelpmp2`, the LSZB position for 2 MB size can be found as log(2 * 1024 * 1024) - 3 = 18 (zero-indexed), therefore the least significant bits will be `b'011 1111 1111 1111 1111`. For `kernelpmp3`, the 4 MB region has to be divided into two regions of 2 MB each and then configured into PMP registers, as LSZB at the 19th position would overflow into the upper address bits. On executing `./run kernelpmp2`, the output is -
```
[*] Success! The xv6 kernel-pmp2 is booting! Yayyy!

Testing PMP Version 2
1: Testing accessible regions..
[*] PASS.
[*] PASS.
[*] PASS.
2: Testing inaccessible regions..
[*] PASS.
[*] PASS.
```

### Secure boot
The last part of the bootloader assignment tasks us with ensuring that the loaded kernel is tamper-proof, by checking the SHA-256 hash of the binary. To do this, we initialize SHA-256 context struct, update its fields by passing the real kernel at `RAMDISK` and calculate the hash. By comparing bytes with the expected hash array in `measurements.h`, we can determine if the kernel has been tampered with.

If it is indeed tampered, then we load the recovery image stored at `RECOVERYDISK 0x84500000`. On executing `./run kernel1tamper`, the output would be -
```
[X] ERROR: Kernel hash DOES NOT MATCH trusted value --> KERNEL IS COMPROMISED!
---------------------------------------------------------------------------------
Expected: 419c2f3e4e8e5069a5a54bde5df0db03c8612a230fc8f7596db185b28adfd64c
Observed: 09b5dd6cf02350ff2d118c70bcf6650dc1f5a7908d211939c75d0212ed59ea24
---------------------------------------------------------------------------------
Goodbye!
```

## Memory
In the second assignment, we make optimizations to memory allocation to processes.

### On-demand allocation
xv6 statically allocates the entire memory required by the process when `exec()` is called on it. The function starts by resolving the file's pathname to an inode (which is an OS-level representation of a file) and validating it as an ELF executable. The L2 page table is created using `proc_pagetable()`. 

For each program header, `uvmalloc()` is used for allocating memory to it. The function takes in the pagetable and the current and desired size of the process's virtual memory as arguments. A page of physical memory is given to the kernel by the `kalloc()` function, and mapped to virtual address of the process using `mappages()`. It also conerts the flag bits into the relevant permission. 

After this, `loadseg()` copies the loadable segments of the executable from disk into the newly allocated memory. It iterates over the segment in page-sized chunks, walks the pagetable using `walkaddr()` to translate to the physical address and reads from the inode using `readi()` into the physical address offset. 

The stack guard and user stack memory are then allocated, arguments are pushed onto it, the gurad's PTE bit is invalidated, the stack pointer is grown by decrementing it by a page, and lastly, the program entry location is updated in the `epc` register.

On-demand allocation is an optimization technique where the application is loaded from disk as required, not all at once. To implement this, we add a new field in the `proc` data structure to keep track if the process requires on-demand loading. Instead of using `uvmalloc()` to give the entire memory, we allow it to incur a page fault.

Note: `init` (the first process that runs after the kernel has booted,  ancestor of all other processes) and `sh` (command line interface) must always be statically allocated.

### Page fault handler
Page faults are a type of exception raised when the process tries to access memory which has not yet been allocated. Since we did not allocate the entire memory required by the process, the exception will be caught by the `usertrap()` function. We can read the `scause` register to find the value of the exception. If it is equal to 12 (instruction page fault), 13 (load page fault) or 15 (store page fault), we should redirect execution to `page_fault_handler()` in `pfault.c`.

In the exact same way as `exec()`, the page fault handler uses `uvmalloc()` and `loadseg()` to allocate memory, but at the granularity of a page size from the faulting address. An additional check is performed whether the faulting address (read from the `stval` register) lies between `ph.vaddr` and `ph.vaddr + ph.memsz`. This process is continued till no more page faults are incurred.

### Page eviction and retrieval from disk
To demonstrate page eviction and retrieval from disk, we are making use of heap memory. The `odheap` and `pswap` test cases use the `sbrk` system call to demand heap memory. These requests utilize the `grow_proc()` function to expand heap by n bytes. Instead of calling `uvmalloc()`, we again let it get redirected to the page fault handler. Also, a new data structure `heap_tracker` in `proc` keeps track of the address, the last load time and its block number on the disk for each of the pages in the array.

If the faulting address belongs to the heap region (found using the heap tracker), we skip the part for process loading from disk. One process, as per the limitations set by the assignment, can only demand at most 100 pages of heap at a time. If it demands more, during a page fault the kernel should evict a victim page to disk and retrieve the requested page from disk. In this assignment, the victim page is selected using the FIFO policy. Whenever a heap page is loaded, its last load time is set as the current timestamp and the  process's number of heap pages is incremented. This way during a page fault, if the maximum number is surpassed a check is conducted to find the index of the page with the earliest load time.

With the victim page selected, it must now be freed from memory and evicted to disk. To do this, we must understand how the virtual storage provided in QEMU is partitioned. A virtual file system image `fs.img` is created using the `mkfs.c` script. It is responsible for defining the layout for the file system by creating the superblock, bitmap, inodes and data blocks. In this assignment, apart from the others, a new area called the Page Swap Area is introduced and 4000 blocks are assigned to it, and placed after the boot block, superblock and log blocks. The block allocation is done using the `balloc()` function. 

Back to the heap page eviction problem, we now have the block abstraction layer at our disposal to write to the disk. A PSA tracker array keeps count of the free PSA blocks. By iterating over it, we can find the first free index and write use 4 adjacent blocks of 1 KB each to write a page. Before writing, the page must be copied from user address space to kernel space using `copyin()`. This is vital for when the kernel needs to access data provided by user processes while maintaining memory protection. It walks the pagetable and safely copies it to a newly allocated kernel page. With this preliminary step completed, we use `memmove` to copy the kernel pages into buffers, and write the buffers into each of the 4 blocks using the `bwrite()` function. The heap tracker and PSA tracker are appropriately updated.

The last part is to retrieve the heap page from the disk. Using the `startblock` field of the heap tracker element, its location on the disk is found. The inverse of the operations above are performed - 4 blocks are read from the start block location into buffers using `bread()`, the buffers are copied into kernel pages and kernel pages are copied to user process address space using `copyout()`. 

Upon starting xv6 using `make qemu` and executing the test case `test-pageswap`, we observe the output -
```
Skipping heap region allocation (proc: sh, addr: 5000, npages: 16)
----------------------------------------
#PF: Proc (sh), Page (5000)
----------------------------------------
#PF: Proc (sh), Page (14000)
Ondemand process creation (proc: test-pageswap)
Skipping program section loading (proc: test-pageswap, addr: 0, size: 4096)
Skipping program section loading (proc: test-pageswap, addr: 1000, size: 32)
----------------------------------------
#PF: Proc (test-pageswap), Page (0)
LOAD: Addr (0), SEG: (1000), SIZE (4096)
Skipping heap region allocation (proc: test-pageswap, addr: 4000, npages: 101)
----------------------------------------
#PF: Proc (test-pageswap), Page (4000)
----------------------------------------
#PF: Proc (test-pageswap), Page (5000)
----------------------------------------
#PF: Proc (test-pageswap), Page (6000)
----------------------------------------
#PF: Proc (test-pageswap), Page (7000)
----------------------------------------
#PF: Proc (test-pageswap), Page (8000)
----------------------------------------
.
.
.
----------------------------------------
#PF: Proc (test-pageswap), Page (66000)
----------------------------------------
#PF: Proc (test-pageswap), Page (67000)
----------------------------------------
#PF: Proc (test-pageswap), Page (68000)
EVICT: Page (4000) --> PSA (0 - 3)
----------------------------------------
#PF: Proc (test-pageswap), Page (4000)
EVICT: Page (5000) --> PSA (4 - 7)
RETRIEVE: Page (4000) --> PSA (0 - 3)
[*] PSWAP TEST PASSED.
```

## Threads
The third assignment assesses our knowledge on the steps taken during context switches between user space and kernel space while scheduling threads.

### The ULThread Library
xv6 processes are only managed by a single kernel-level thread. In this assignment, a new file `ulthread.c` is created which acts as the user-level threading library by separating user threads and supporting operations for creating, initializing, scheduling, yielding and destroying them. To do this, a data structure keeps track of - 1. thread id, 2. state, 3. priority, 4. last scheduled tine, 5. context and 6. stack pointer.

We create an array of this struct for the maximum number of threads allowed, and one extra as the scheduler thread. All threads in the array are initially assigned as free, whereas the scheduler thread is assigned as runnable. When a test case invokes the function to create a thread, the first free thread in the array is chosen. Its tid is set as the value of a global variable keeping track of tids, and the global variable is incremented for the next thread. Its state is set to runnable and other contents of the thread data structure are filled appropriately. The stack, return address and arguments are passed as arguments by the test case. The stack pointer grows downward by the size of one page. A global variable keeping track of the number of runnable threads is incremented.

### Context switching
Before moving on to thread scheduling, we must set up the necessary framework to switch the context between the scheduler thread and the currently executing thread. Without proper handling of registers and incorporating a save space for them during context switches, the stack/return address could point to incorrect locations, which would jeopardize the correct execution of the threads. 

The context data structure here is basically equivalent to the trapframe of a process. It consists of the `ra`, `sp`, `s0` - `s11` and `a0` - `a5` callee-saved registers. Similar to the kernel space - user space context switch, we must also set up the trampoline code in `user_swtch.S` for storing the context of the yielded thread and loading the context of the scheduler thread. It stores the registers at 64 bit offsets from `a0` and loads them from 64 bit offsets from `a1`. 

Note: The trampoline executed at system calls or interrupts must handle many additional things, as it is the trusted code (despite being in user address space) which is responsible for changing to supervisor mode. It has to take care of privileged registers for kernel page table, hart ID, etc. which is not required in our case, as we are not switching privilege levels or invoking the kernel between threads.

### Thread scheduling
The thread scheduler runs in a loop while the number of runnable threads are greater than 0. The index of the thread to be scheduled next depends on the scheduling algorithm used. We iterate over the threads array and keep updating the index based on the scheduling condition - for FCFS and round robin we check whose last scheduled time is the earliest, whereas in case of priority, we whose priority is the greatest. The `current_thread` pointer is updated with the new thread and its state is set to running. The scheduler thread's state is set to runnable. Finally, the context switch code is called and pointers to the scheduler thread's context and the user thread's context (selected by the algorithm) are passed as arguments. The former is paused and the latter is resumed.

The user thread which is currently executing can cooperatively yield its execution to give CPU time to others. Only in the case of FCFS, it will not yield because it performs to completion. In the other two algorithms, during yield, the last scheduled time is updated using a new system call `ctime()`, which will later be used by the scheduler to make decisions (The functionality of `ctime()` is implemented in `syscall.c`, and it is used for global timekeeping). Once again, the context switch code is called, however this time the arguments are passed in the inverse order to pause the user thread and resume the scheduler thread.

If a thread has completed execution, it destroys itself by setting its state as free and decrementing the global variable keeping track of the number of runnable threads.

For example, the round robin test gives the following output -
```
[*] ultcreate(tid: 1, ra: 0x0000000000000000, sp: 0x0000000000002030)
[*] ultcreate(tid: 2, ra: 0x0000000000000000, sp: 0x0000000000003030)
[*] ultcreate(tid: 3, ra: 0x0000000000000000, sp: 0x0000000000004030)
[*] ultschedule (next tid: 1)
[.] started the thread function (tid = 1, a1 = 1)
[*] ultyield(tid: 1)
[*] ultschedule (next tid: 2)
[.] started the thread function (tid = 2, a1 = 1)
[*] ultyield(tid: 2)
[*] ultschedule (next tid: 3)
[.] started the thread function (tid = 3, a1 = 1)
[*] ultyield(tid: 3)
[*] ultschedule (next tid: 1)
[*] ultyield(tid: 1)
[*] ultschedule (next tid: 2)
[*] ultyield(tid: 2)
[*] ultschedule (next tid: 3)
[*] ultyield(tid: 3)
[*] ultschedule (next tid: 1)
[*] ultyield(tid: 1)
[*] ultschedule (next tid: 2)
[*] ultyield(tid: 2)
[*] ultschedule (next tid: 3)
[*] ultyield(tid: 3)
[*] ultschedule (next tid: 1)
[*] ultyield(tid: 1)
[*] ultschedule (next tid: 2)
[*] ultyield(tid: 2)
[*] ultschedule (next tid: 3)
[*] ultyield(tid: 3)
[*] ultschedule (next tid: 1)
[*] ultyield(tid: 1)
[*] ultschedule (next tid: 2)
[*] ultyield(tid: 2)
[*] ultschedule (next tid: 3)
[*] ultyield(tid: 3)
[*] ultschedule (next tid: 1)
[*] ultdestroy(tid: 1)
[*] ultschedule (next tid: 2)
[*] ultdestroy(tid: 2)
[*] ultschedule (next tid: 3)
[*] ultdestroy(tid: 3)
[*] User-Level Threading Test #4 (RR Collaborative) Complete.
```

## Virtualization
In the fourth assignment, we virtualize xv6 by trapping privileged instructions and emulating them in software.

### VM structure
Our target is to ensure that the virtual xv6 kernel defined in the `vm` directory runs as a user process managed by the host xv6 kernel. The guest can execute unprivileged instructions directly, but privileged instructions must be trapped to the virtual machine monitor. To maintain the VM's registers in-memory for machine trap handling, machine setup trap, machine information state, supervisor page table register and supervisor trap setup, a data structure is defined which contains a map of these registers and the current privilege mode.

The `Makefile` is used to generate the test binary which spawns the VM. Each object file in `$(VM)` directory is compiled using GCC or GAS, which are then linked together to form `$(VM)/vm`. The linker script `$V$/vm.ld` indicates the entry point, which is contained in `entry.S` and then jumps to `start.c`, just like in the bootloader. The VM binary is copied to the user directory so that we can execute it from the host kernel using QEMU. Booting up the VM involves various privileged instructions like reading and writing to privileged registers, and changing privilege levels, which must be emulated.

In `usertrap()`, if a trap originates from the VM process, it is redirected to the `trap_and_emulate()` function.

### Decoding instructions

Each RISC-V instruction is 4 bytes. The instructions we have to emulate are of I-type, which have the following bit structure -


| [31:20] | [19:15] | [14:12] | [11:7] | [6:0] |
|-|-|-|-|-|
| `uimm` | `rs1` | `funct3` | `rd` | `opcode` |

We can get the instruction that caused the trap by fetching the virtual address from the `sepc` register and walking the page table. The above values are extracted using bit shifts.

The purpose of the instruction can be decoded by looking at the above fields. For example, by inspecting the objdump, we can decode the first privileged instruction when booting up the VM - guest executes at VA `0xa`: `csrr a1, mhartid`. The VMM reads the VA and finds the binary code `0xf14025f3`. Extracting the values, we get `uimm = 0xf14` (code for `mhartid` register), `rs1 = 0x0` (source register 1), `funct3 = 0x2` (narrows down the instruction type for a given opcode family), `rd = 0xb` (destination register, code for `a1` register), `opcode = 0x73` (this is the opcode for all CSR instructions).

To extract the register from the `uimm`, in the VM data structure we have kept a map whose key is the `uimm` and value is the corresponding register's privilege mode and value.

### Emulating instructions

We are tasked with emulating 5 instructions - `ecall`, `sret`, `mret`, `csrw` and `csrr`.

For `ecall`, `funct3 = 0x0` and `uimm = 0x0`. The current program counter is saved in the virtual `sepc`, and program counter is updated to the value of the virtual `stvec`, which allows host to handle the system call executed by the guest. Privilege mode is set to S-mode.

For `sret`, `funct3 = 0x0`, `uimm = 0x102` and privilege mode should be S-mode. After returning from a trap, the privilege mode transitions to the value stored in SPP. Clearing the SPP bit ensures the processor doesn’t reapply the previous privilege level on future traps. SIE controls whether interrupts are enabled in supervisor mode. SPIE holds the value of SIE before the trap occurred. During `sret`, SIE is set to SPIE because it restores the interrupt enable state to what it was before the trap occurred. If interrupts were enabled before the trap, they should be re-enabled upon returning. If they were disabled, they should remain disabled. SPIE is set to 1 because it prepares the interrupt state for future traps. When a new trap occurs, SPIE will capture the value of SIE at that moment, and this ensures SPIE is always set correctly in advance. Then, it writes the updated `sstatus` back and sets the program counter to the value of `sepc`, effectively returning to the previous context.

For `mret`, `funct3 = 0x0`, `uimm = 0x302` and privilege mode should be M-mode. For the same reason as above, the `mstatus` register is taken, it's `MPP` bits are cleared, `MIE` bit is set to `MPIE` and `MPIE` is set to 1. Program counter is updated with the value in `mepc`.

For `csrw`, `funct3 = 0x1`, `uimm = 0x302` and privilege mode should be equal or higher than the privilege mode of the register being written to. It moves the value from an unprivileged register (encoded by `rs1`) to a privileged one (encoded by `uimm`). Then it advances the program counter by the length of an instruction.

For `csrr`, `funct3 = 0x1`, `uimm = 0x302` and privilege mode should be equal or higher than the privilege mode of the register being read from. It moves the value from a privileged register (encoded by `uimm`) to an unprivileged one (encoded by `rd`). As above, increases program counter by 4.

If an invalid instruction is trapped, the VM is killed. The output on running the VM inside QEMU is -
```
Created a VM process and allocated memory region (0x0000000080000000 - 0x0000000080400000).
(csrr at 0x000000000000000a) op = 73, rd = b, funct3 = 2, rs1 = 0, uimm = f14
(csrr at 0x000000000000002c) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = f14
(csrr at 0x0000000000000034) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = 300
(csrw at 0x0000000000000048) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 300
(csrw at 0x000000000000004e) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 180
(csrw at 0x000000000000005a) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 341
(csrw at 0x0000000000000062) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 302
(csrw at 0x0000000000000066) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 303
(csrr at 0x000000000000006a) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = 104
(csrw at 0x0000000000000072) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 104
(mret at 0x0000000000000076) op = 73, rd = 0, funct3 = 0, rs1 = 0, uimm = 302
(csrw at 0x0000000000000378) op = 73, rd = 0, funct3 = 1, rs1 = d, uimm = 105
(csrr at 0x0000000000000380) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = 100
(csrw at 0x000000000000038c) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 100
(csrw at 0x0000000000000394) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 141
(sret at 0x0000000000000398) op = 73, rd = 0, funct3 = 0, rs1 = 0, uimm = 102
(ecall at 0x0000000000000422)
(csrr at 0x00000000000003aa) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = 141
(csrw at 0x0000000000000378) op = 73, rd = 0, funct3 = 1, rs1 = d, uimm = 105
(csrr at 0x0000000000000380) op = 73, rd = f, funct3 = 2, rs1 = 0, uimm = 100
(csrw at 0x000000000000038c) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 100
(csrw at 0x0000000000000394) op = 73, rd = 0, funct3 = 1, rs1 = f, uimm = 141
(sret at 0x0000000000000398) op = 73, rd = 0, funct3 = 0, rs1 = 0, uimm = 102
```
