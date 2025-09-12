// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct bucket{
  struct spinlock lock;
  struct buf head;
};

struct {
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct bucket bucket[NBUCKET];
} bcache;

static uint hash_v(uint key) {
  return key % NBUCKET;
}

static void initbucket(struct bucket* b) {
  initlock(&b->lock, "bcache.bucket");// 初始化桶锁
  b->head.prev = &b->head;
  b->head.next = &b->head;
}

void binit(void) {
  for (int i = 0; i < NBUF; ++i) {
    initsleeplock(&bcache.buf[i].lock, "buffer"); // 每个 buf 数据锁
  }
  for (int i = 0; i < NBUCKET; ++i) {
    initbucket(&bcache.bucket[i]);// 每个桶初始化
  }
}

// 获取指定 (dev, blockno) 的缓存块，如果不存在则分配一个新的
static struct buf*
bget(uint dev, uint blockno)
{
  uint v = hash_v(blockno); // 计算桶号
  struct bucket* bucket = &bcache.bucket[v];
  acquire(&bucket->lock);// 加桶锁，保护链表与元数据
// 1. 在桶链表中查找目标块
  for (struct buf *buf = bucket->head.next; buf != &bucket->head;buf = buf->next) {
    if (buf->dev == dev && buf->blockno == blockno) {
      buf->refcnt++;// 命中：增加计数
      release(&bucket->lock);// 立刻放桶锁
      acquiresleep(&buf->lock);// 现在才拿“数据锁”，允许睡眠
      return buf;
    }
  }

  // 未命中
  // 从全局池找一个空闲 buf
  for (int i = 0; i < NBUF; ++i) {
    //无锁抢占一个空闲 buf 槽位
    if (!bcache.buf[i].used &&!__atomic_test_and_set(&bcache.buf[i].used, __ATOMIC_ACQUIRE)) {
      struct buf *buf = &bcache.buf[i];
      buf->dev = dev;
      buf->blockno = blockno;
      buf->valid = 0;
      buf->refcnt = 1;
      buf->next = bucket->head.next;
      buf->prev = &bucket->head;
      bucket->head.next->prev = buf;
      bucket->head.next = buf;
      release(&bucket->lock);
      acquiresleep(&buf->lock);
      return buf;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;
//(dev, blockno)磁盘设备号和逻辑块号
  b = bget(dev, blockno);// (dev, blockno) 就是缓冲区缓存的键（唯一标识）
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  uint v = hash_v(b->blockno);
  struct bucket* bucket = &bcache.bucket[v];
  acquire(&bucket->lock);

  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    __atomic_clear(&b->used, __ATOMIC_RELEASE);
  }

  release(&bucket->lock);
}

void
bpin(struct buf *b) {
  uint v = hash_v(b->blockno);
  struct bucket* bucket = &bcache.bucket[v];
  acquire(&bucket->lock);
  b->refcnt++;
  release(&bucket->lock);
  
}

void
bunpin(struct buf *b) {
  uint v = hash_v(b->blockno);
  struct bucket* bucket = &bcache.bucket[v];
  acquire(&bucket->lock);
  b->refcnt--;
  release(&bucket->lock);
}


