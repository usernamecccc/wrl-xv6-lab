#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

extern pte_t* walk(pagetable_t pagetable, uint64 va, int alloc);

uint64
sys_exit(void)
{
  int n;
  if(argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if(argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}


#ifdef LAB_PGTBL
// sys_pgaccess(base, len, user_mask)
// - base: 从哪个用户虚拟地址开始检查
// - len:  要检查多少页（建议最多64）
// - user_mask: 用户缓冲区地址，用来接收位图结果（uint64）
uint64
sys_pgaccess(void)
{
  uint64 base;        // arg0
  int len;            // arg1
  uint64 user_mask;   // arg2

  if (argaddr(0, &base) < 0 || argint(1, &len) < 0 || argaddr(2, &user_mask) < 0)
    return -1;

  if (len < 0)
    return -1;
  if (len > 64)
    len = 64;                 // 上限64页

  struct proc *p = myproc();
  uint64 mask = 0;

  for (int i = 0; i < len; i++) {
    uint64 va = PGROUNDDOWN(base + (uint64)i * PGSIZE);
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte == 0)             // 未映射：对应位为0，跳过
      continue;
    if ((*pte & PTE_V) == 0)  // 无效：跳过
      continue;
    if ((*pte & PTE_U) == 0)  // 非用户页：跳过
      continue;

    if (*pte & PTE_A) {
      mask |= (1ULL << i);    // 置位（注意 1ULL，避免高位截断）
      *pte &= ~PTE_A;         // 清除访问位，保证“增量”语义
    }
  }

  int bytes = (len + 7) / 8;
  if (bytes == 0) bytes = 1; 
  if (copyout(p->pagetable, user_mask, (char *)&mask, bytes) < 0)
    return -1;

  return 0;
}
#endif


uint64
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
