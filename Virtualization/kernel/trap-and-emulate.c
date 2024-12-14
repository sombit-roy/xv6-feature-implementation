#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "stdbool.h"
#include "stdlib.h"

#define U_MODE 0
#define S_MODE 1
#define M_MODE 2

// Struct to keep VM registers (Sample; feel free to change.)
struct vm_reg {
    int     mode;
    uint64  val;
};

// Keep the virtual state of the VM's privileged registers
struct vm_virtual_state {
    struct vm_reg vm_reg_map[0xfff];
    int privilege_mode;
};

struct vm_virtual_state vm_state;

void trap_and_emulate(void) {
    /* Comes here when a VM tries to execute a supervisor instruction. */
    struct proc *p = myproc();
    uint64 virtual_addr = r_sepc();
    uint32 instruction = *((uint32*)(walkaddr(p->pagetable, virtual_addr) | (virtual_addr & 0xFFF)));
    uint32 op = instruction & 0x7F;
    uint32 rd = (instruction >> 7) & 0x1F;
    uint32 funct3 = (instruction >> 12) & 0x7;
    uint32 rs1 = (instruction >> 15) & 0x1F;
    uint32 uimm = (instruction >> 20) & 0xFFF;

    if (funct3 == 0x0 && uimm == 0) {
        printf("(ecall at %p)\n", p->trapframe->epc);
        vm_state.vm_reg_map[0x141].val = p->trapframe->epc;
        p->trapframe->epc = vm_state.vm_reg_map[0x105].val;
        vm_state.privilege_mode = S_MODE;
    } else if (funct3 == 0x0 && uimm == 0x102 && vm_state.privilege_mode >= S_MODE) {
        printf("(sret at %p) op = %x, rd = %x, funct3 = %x, rs1 = %x, uimm = %x\n", virtual_addr, op, rd, funct3, rs1, uimm);
        uint64 sstatus = vm_state.vm_reg_map[0x100].val;
        vm_state.privilege_mode = ((sstatus >> 8) & 0x1) ? S_MODE : U_MODE;
        
        sstatus &= ~(1 << 8); // Clear the SPP bit
        sstatus |= ((sstatus >> 5) & 0x1) << 1; // set SIE bit to SPIE
        sstatus &= ~(1 << 5); // set SPIE bit to 1
        
        vm_state.vm_reg_map[0x100].val = sstatus;
        p->trapframe->epc = vm_state.vm_reg_map[0x141].val;
    } else if (funct3 == 0x0 && uimm == 0x302 && vm_state.privilege_mode >= M_MODE) {
        printf("(mret at %p) op = %x, rd = %x, funct3 = %x, rs1 = %x, uimm = %x\n", virtual_addr, op, rd, funct3, rs1, uimm);
        uint64 mstatus = vm_state.vm_reg_map[0x300].val;
        vm_state.privilege_mode = ((mstatus >> 11) & 0x1) ? S_MODE : U_MODE;
        
        mstatus &= ~MSTATUS_MPP_MASK; // clear MPP bits
        mstatus |= ((mstatus >> 7) & 0x1) << 3; // set MIE bit to MPIE
        mstatus &= (1 << 0x7); // set MPIE bit to 1
        
        vm_state.vm_reg_map[0x300].val = mstatus; // write mstatus register
        p->trapframe->epc = vm_state.vm_reg_map[0x341].val; // set the program counter to the value of mepc
    } else if (funct3 == 0x1 && vm_state.privilege_mode >= vm_state.vm_reg_map[uimm].mode) {
        printf("(csrw at %p) op = %x, rd = %x, funct3 = %x, rs1 = %x, uimm = %x\n", virtual_addr, op, rd, funct3, rs1, uimm);
        uint64* rs1_reg= &(p->trapframe->ra) + rs1 - 1;
        vm_state.vm_reg_map[uimm].val = *rs1_reg;
        p->trapframe->epc += 4;
    } else if (funct3 == 0x2 && vm_state.privilege_mode >= vm_state.vm_reg_map[uimm].mode) {
        printf("(csrr at %p) op = %x, rd = %x, funct3 = %x, rs1 = %x, uimm = %x\n", virtual_addr, op, rd, funct3, rs1, uimm);
        uint64* rd_reg = &(p->trapframe->ra) + rd - 1;
        *rd_reg = vm_state.vm_reg_map[uimm].val;
        p->trapframe->epc += 4;
    } else {
        setkilled(p);
    }
}

void trap_and_emulate_init(void) {
    
    int m_mode_reg[21] = {0xf11/*mvendorid*/, 0xf12/*marchid*/, 0xf13/*mimpid*/, 0xf14/*mhartid*/, 0x300/*mstatus*/, 0x301/*misa*/, 0x302/*medeleg*/, 0x303/*mideleg*/, 0x304/*mie*/, 0x305/*mtvec*/, 0x306/*mcounteren*/, 0x340/*mscratch*/, 0x341/*mepc*/, 0x342/*mcause*/, 0x343/*mtval*/, 0x344/*mip*/};
    int s_mode_reg[12] = {0x100/*sstatus*/, 0x102/*sedeleg*/, 0x103/*sideleg*/, 0x104/*sie*/, 0x105/*stvec*/, 0x106/*scounteren*/, 0x140/*sscratch*/, 0x141/*sepc*/, 0x142/*scause*/, 0x143/*stval*/, 0x144/*sip*/, 0x180/*satp*/};
    
    vm_state.vm_reg_map[m_mode_reg[0]] = (struct vm_reg){ M_MODE, 0x637365353336 };
    for (int i = 1; i < 21; i++) {
        vm_state.vm_reg_map[m_mode_reg[i]] = (struct vm_reg){ M_MODE, 0x0 };
    }
    for (int i = 0; i < 12; i++) {
        vm_state.vm_reg_map[s_mode_reg[i]] = (struct vm_reg){ S_MODE, 0x0 };
    }

    vm_state.privilege_mode = M_MODE;
}
