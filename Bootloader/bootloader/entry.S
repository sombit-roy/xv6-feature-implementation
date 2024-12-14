# _entry should be the first function invoked when
# the QEMU CPU starts executing. It's location should
# according to the memory layout provided in param.h

.section .text
.global _entry
_entry:
        # task: setup a per-CPU stack for C and jump to start() function

	csrr t0, mhartid

	# load base_address of bl_stack
	la t1, bl_stack

	# calculate stack offset - *bl_stack + (hartID * STSIZE)
	li t2, 4096
	mul t0, t0, t2
	add t1, t1, t0

	# set stack pointer to top - stack grows downwards
	li t0, 4096	
	add sp, t1, t0

	# jump to start function
	la t0, start
	jr t0

spin:
        j spin
