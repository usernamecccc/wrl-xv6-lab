// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];


static void initmemlock() {
  for (int i = 0; i < NCPU; ++i) {
    initlock(&kmem[i].lock, "kmem");
  }
}
void
kinit()
{
  //initlock(&kmem.lock, "kmem");
  initmemlock();
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  // 校验释放页是否合法：必须页对齐，不能小于内核 end，不能超出 PHYSTOP 
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 用 1 填充整页，
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
// push_off/pop_off 保证 cpuid() 返回正确的 CPU id
  // 因为 cpuid() 在中断发生时可能读到错误值
  push_off();
  int id=cpuid();// 获取当前 CPU id 
  // 将该物理页挂回当前 CPU 的 freelist
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;
// 同样需要关闭中断，保证 cpuid() 返回的 CPU id 稳定
  push_off();
  int id=cpuid();// 获取当前 CPU id 
  // 1. 优先从本 CPU freelist 分配 
  acquire(&kmem[id].lock);

  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;// 取出一页 
  release(&kmem[id].lock);//释放锁
  if(!r)// 2. 如果本地 freelist 为空，则遍历其他 CPU freelist，尝试偷取一页 
  {
     for (int i = 0; i < NCPU; ++i) {
      if (i == id)// 跳过自己
        continue;
      acquire(&kmem[i].lock);
      r = kmem[i].freelist;
      if (r) {
        kmem[i].freelist = r->next;
        release(&kmem[i].lock); // 成功偷到一页 
        break;
      }
      release(&kmem[i].lock); // 没偷到，释放锁继续下一个
    }
  }
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
