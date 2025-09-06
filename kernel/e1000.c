#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "e1000_dev.h"
#include "net.h"

#define TX_RING_SIZE 16
static struct tx_desc tx_ring[TX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *tx_mbufs[TX_RING_SIZE];

#define RX_RING_SIZE 16
static struct rx_desc rx_ring[RX_RING_SIZE] __attribute__((aligned(16)));
static struct mbuf *rx_mbufs[RX_RING_SIZE];

// remember where the e1000's registers live.
static volatile uint32 *regs;

struct spinlock e1000_lock;

// called by pci_init().
// xregs is the memory address at which the
// e1000's registers are mapped.
void
e1000_init(uint32 *xregs)
{
  int i;

  initlock(&e1000_lock, "e1000");

  regs = xregs;

  // Reset the device
  regs[E1000_IMS] = 0; // disable interrupts
  regs[E1000_CTL] |= E1000_CTL_RST;
  regs[E1000_IMS] = 0; // redisable interrupts
  __sync_synchronize();

  // [E1000 14.5] Transmit initialization
  memset(tx_ring, 0, sizeof(tx_ring));
  for (i = 0; i < TX_RING_SIZE; i++) {
    tx_ring[i].status = E1000_TXD_STAT_DD;
    tx_mbufs[i] = 0;
  }
  regs[E1000_TDBAL] = (uint64) tx_ring;
  if(sizeof(tx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_TDLEN] = sizeof(tx_ring);
  regs[E1000_TDH] = regs[E1000_TDT] = 0;
  
  // [E1000 14.4] Receive initialization
  memset(rx_ring, 0, sizeof(rx_ring));
  for (i = 0; i < RX_RING_SIZE; i++) {
    rx_mbufs[i] = mbufalloc(0);
    if (!rx_mbufs[i])
      panic("e1000");
    rx_ring[i].addr = (uint64) rx_mbufs[i]->head;
  }
  regs[E1000_RDBAL] = (uint64) rx_ring;
  if(sizeof(rx_ring) % 128 != 0)
    panic("e1000");
  regs[E1000_RDH] = 0;
  regs[E1000_RDT] = RX_RING_SIZE - 1;
  regs[E1000_RDLEN] = sizeof(rx_ring);

  // filter by qemu's MAC address, 52:54:00:12:34:56
  regs[E1000_RA] = 0x12005452;
  regs[E1000_RA+1] = 0x5634 | (1<<31);
  // multicast table
  for (int i = 0; i < 4096/32; i++)
    regs[E1000_MTA + i] = 0;

  // transmitter control bits.
  regs[E1000_TCTL] = E1000_TCTL_EN |  // enable
    E1000_TCTL_PSP |                  // pad short packets
    (0x10 << E1000_TCTL_CT_SHIFT) |   // collision stuff
    (0x40 << E1000_TCTL_COLD_SHIFT);
  regs[E1000_TIPG] = 10 | (8<<10) | (6<<20); // inter-pkt gap

  // receiver control bits.
  regs[E1000_RCTL] = E1000_RCTL_EN | // enable receiver
    E1000_RCTL_BAM |                 // enable broadcast
    E1000_RCTL_SZ_2048 |             // 2048-byte rx buffers
    E1000_RCTL_SECRC;                // strip CRC
  
  // ask e1000 for receive interrupts.
  regs[E1000_RDTR] = 0; // interrupt after every received packet (no timer)
  regs[E1000_RADV] = 0; // interrupt after every packet (no timer)
  regs[E1000_IMS] = (1 << 7); // RXDW -- Receiver Descriptor Write Back
}

int e1000_transmit(struct mbuf *m)
{
    // 获取锁，确保多线程环境下的同步
    acquire(&e1000_lock);

    // 读取E1000_TDT控制寄存器，获取下一个可用的发送描述符索引
    int index = regs[E1000_TDT];

    // 检查当前索引位置的发送描述符是否已完成发送（通过检查E1000_TXD_STAT_DD位）
    if ((tx_ring[index].status & E1000_TXD_STAT_DD) == 0) {
        // 如果发送未完成，释放锁并返回错误
        release(&e1000_lock);
        return -1;
    }

    // 如果当前索引位置有上一个发送的mbuf，释放它
    if (tx_mbufs[index])
        mbuffree(tx_mbufs[index]);

    // 将当前要发送的mbuf保存到发送缓冲区数组中
    tx_mbufs[index] = m;
    // 设置发送描述符的长度字段为当前mbuf的长度
    tx_ring[index].length = m->len;
    // 设置发送描述符的地址字段为当前mbuf数据的首地址
    tx_ring[index].addr = (uint64)m->head;

    // 设置发送描述符的命令字段，包括报告状态（E1000_TXD_CMD_RS）和数据包结束（E1000_TXD_CMD_EOP）
    tx_ring[index].cmd = E1000_TXD_CMD_RS | E1000_TXD_CMD_EOP;

    // 更新E1000_TDT控制寄存器，指向下一个可用的发送描述符索引（环状结构，取模运算）
    regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;

    // 释放锁
    release(&e1000_lock);

    // 返回成功
    return 0;
}

static void e1000_recv(void)
{
    // 无限循环遍历接收描述符环，处理所有已接收的数据包
    while (1) {

        // 获取下一个接收描述符的索引，环状结构，取模运算
        int index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;

        // 检查当前描述符的状态位，判断是否有新的数据包
        if ((rx_ring[index].status & E1000_RXD_STAT_DD) == 0) {
            // 如果没有新的数据包，退出循环
            return;
        }

        // 设置当前缓冲区的长度为接收描述符中记录的长度
        rx_mbufs[index]->len = rx_ring[index].length;

        // 将接收到的数据包传递给网络层处理
        net_rx(rx_mbufs[index]);

        // 分配新的mbuf缓冲区，以便接收新的数据包
        rx_mbufs[index] = mbufalloc(0);
        // 检查分配是否成功
        if (!rx_mbufs[index])
            panic("e1000_recv: mbufalloc failed");

        // 清除当前描述符的状态位
        rx_ring[index].status = 0;

        // 更新描述符的地址为新分配的缓冲区地址
        rx_ring[index].addr = (uint64)rx_mbufs[index]->head;

        // 更新接收描述符尾指针，通知硬件描述符已处理
        regs[E1000_RDT] = index;
    }
}


void
e1000_intr(void)
{
  // tell the e1000 we've seen this interrupt;
  // without this the e1000 won't raise any
  // further interrupts.
  regs[E1000_ICR] = 0xffffffff;

  e1000_recv();
}
