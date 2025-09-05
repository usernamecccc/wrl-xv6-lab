// Physical memory allocator, for user processes,
// kernel stacks, page-table pages, and pipe buffers.
// Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

extern char end[]; // first address after kernel. defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// ---- Reference counting for physical pages ----
struct spinlock reflock;
static int referencecount[PHYSTOP/PGSIZE];   // 用 int，避免下溢回绕

static inline uint64 pa2idx(uint64 pa) { return pa / PGSIZE; }

void incref(uint64 pa){
  acquire(&reflock);
  referencecount[pa2idx(pa)]++;
  release(&reflock);
}

int decref(uint64 pa){
  acquire(&reflock);
  int c = --referencecount[pa2idx(pa)];
  release(&reflock);
  return c;
}

int getref(uint64 pa){
  acquire(&reflock);
  int c = referencecount[pa2idx(pa)];
  release(&reflock);
  return c;
}

static void freerange(void *pa_start, void *pa_end);

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&reflock, "ref");
  freerange(end, (void*)PHYSTOP);
}

// Build the initial free list.
// 用“地址值本身”作为物理地址来索引 refcnt。
// 先设 ref=1，再 kfree()，kfree 的 -- 恰好变 0 并入 freelist。
static void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    acquire(&reflock);
    referencecount[pa2idx((uint64)p)] = 1;
    release(&reflock);
    kfree(p);
  }
}

// Free the page at KVA（xv6 内核直映：地址值即 PA）。
// 正常路径：减引用；为 0 则真正回收到 freelist。
void
kfree(void *kva)
{
  uint64 a = (uint64)kva;

  if((a % PGSIZE) != 0 || (char*)kva < end || a >= PHYSTOP)
    panic("kfree");

  if(decref(a) > 0)
    return; // 还有共享者，不回收

  // 真回收
  memset(kva, 1, PGSIZE);
  struct run *r = (struct run*)kva;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page. Return KVA（==PA）or 0.
void*
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    memset((char*)r, 5, PGSIZE);

    acquire(&reflock);
    referencecount[pa2idx((uint64)r)] = 1;   // 新页 ref=1（用地址值索引）
    release(&reflock);
  }
  return (void*)r;
}

