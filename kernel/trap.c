#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

pte_t* walk(pagetable_t pagetable, uint64 va, int alloc);

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;
// 如果是从内核态进入，说明出了严重错误
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");
// 将 stvec 改成内核的 trap 处理入口
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();

   // 保存用户程序的 sepc（触发异常的 PC）
  p->trapframe->epc = r_sepc();

  if(r_scause() == 8){
    // 系统调用
    if(p->killed)
      goto out;

    // ecall 下一条指令作为返回点
    p->trapframe->epc += 4;

    intr_on();
    syscall();

  } else if(r_scause() == 12 || r_scause() == 15){
    // 12: instruction page fault (按你的要求一并走这里)
    // 15: store/AMO page fault（典型 COW）

    uint64 va = r_stval();
    if(va >= MAXVA){
      p->killed = 1;
      goto out;
    }

    uint64 va0 = PGROUNDDOWN(va);
    pte_t *pte = walk(p->pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0){
      p->killed = 1;
      goto out;
    }

    if((*pte & PTE_COW) == 0){
      // 不是 COW 页（例如真正非法访问）——按题意杀掉
      p->killed = 1;
      goto out;
    }

    // COW 拆页
    uint64 oldpa = PTE2PA(*pte);
    char *mem = kalloc();
    if(mem == 0){
      p->killed = 1;  // OOM，按题意 kill
      goto out;
    }

    // 复制旧页到新页（PA 直映，无需 +KERNBASE）
    memmove(mem, (void*)oldpa, PGSIZE);

    // 替换映射：先 unmap（会对旧页 kfree，从而递减引用计数）
    uint flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
    uvmunmap(p->pagetable, va0, 1, 1);

    // 重新映射到新页（kalloc() 返回值可直接作为 pa）
    if(mappages(p->pagetable, va0, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      panic("cowhandler: mappages failed");
    }

    sfence_vma();

  } else if((which_dev = devintr()) != 0){
    // device interrupt
    // ok
  } else {
    // unexpected trap
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

out:
  if(p->killed)
    exit(-1);

  if(which_dev == 2)
    yield();

  usertrapret();
}



//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();
// 准备切换 trap 向量到 usertrap()，所以先关中断
  intr_off();

   // 设置用户态 trap 入口（trampoline.S）
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // trapframe 保存内核必要信息，供用户态异常时恢复
  p->trapframe->kernel_satp = r_satp();          // 内核页表
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // 内核栈顶
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  // 设置 sstatus：返回用户态并开中断
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

