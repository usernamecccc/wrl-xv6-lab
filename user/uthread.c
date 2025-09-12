#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* Possible states of a thread: */
#define FREE        0x0
#define RUNNING     0x1
#define RUNNABLE    0x2

#define STACK_SIZE  8192
#define MAX_THREAD  4

// 线程的上下文
struct thread_context {
  uint64 ra;  // 返回地址寄存器
  uint64 sp;  // 栈指针寄存器

  uint64 s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;  // 保存s寄存器
};

struct thread {
  char stack[STACK_SIZE];  // 线程的栈
  int state;               // 线程状态：FREE, RUNNING, RUNNABLE

  struct thread_context context;  // 线程的上下文，切换时保存/恢复用
};

struct thread all_thread[MAX_THREAD];
struct thread *current_thread;
extern void thread_switch(uint64, uint64);
              
void 
thread_init(void)
{
  // main() is thread 0, which will make the first invocation to
  // thread_schedule().  it needs a stack so that the first thread_switch() can
  // save thread 0's state.  thread_schedule() won't run the main thread ever
  // again, because its state is set to RUNNING, and thread_schedule() selects
  // a RUNNABLE thread.
  current_thread = &all_thread[0];
  current_thread->state = RUNNING;
}

void thread_schedule(void) {
  struct thread *t, *next_thread;

  // 查找下一个可运行的线程
  next_thread = 0;
  t = current_thread + 1;
  for (int i = 0; i < MAX_THREAD; i++) {//找下一个 RUNNABLE
      if (t >= all_thread + MAX_THREAD)
          t = all_thread; // 环回
      if (t->state == RUNNABLE) {// 命中一个可运行线程
          next_thread = t;  // 找到可运行线程
          break;
      }
      t = t + 1;
  }
// 没有可运行线程，直接退出
  if (next_thread == 0) {
      printf("thread_schedule: no runnable threads\n");
      exit(-1);
  }

  if (current_thread != next_thread) {  // 如果当前线程和下一个线程不同，进行切换
      next_thread->state = RUNNING;  // 设置下一个线程为运行状态
      t = current_thread;
      current_thread = next_thread;
      // 调用 thread_switch 实现上下文切换
      // 真正的上下文切换：把 t->context 保存，把 current_thread->context 恢复
    //    恢复后，CPU 的 ra/sp/s0..s11 变成 next 的，上下文回到它上次停的位置；
    //    对“首次运行”的线程，ra = func，因此会从 func 开始执行。
      thread_switch((uint64)&t->context, (uint64)&current_thread->context);
  } else {
      next_thread = 0;  // 没有线程需要切换
  }
}


void thread_create(void (*func)()) {
  struct thread *t;

  for (t = all_thread; t < all_thread + MAX_THREAD; t++) {
      if (t->state == FREE) break;  // 找到一个空闲的线程
  }
  t->state = RUNNABLE;  // 将线程状态设置为可运行

  // 初始化线程上下文
  t->context.ra = (uint64)func;  // 设置返回地址，指向传入的函数（线程的执行函数）
  t->context.sp = (uint64)(t->stack + STACK_SIZE);  // 设置栈指针，指向栈的顶部
}


void 
thread_yield(void)
{
  current_thread->state = RUNNABLE;
  thread_schedule();
}

volatile int a_started, b_started, c_started;
volatile int a_n, b_n, c_n;

void 
thread_a(void)
{
  int i;
  printf("thread_a started\n");
  a_started = 1;
  while(b_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_a %d\n", i);
    a_n += 1;
    thread_yield();
  }
  printf("thread_a: exit after %d\n", a_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_b(void)
{
  int i;
  printf("thread_b started\n");
  b_started = 1;
  while(a_started == 0 || c_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_b %d\n", i);
    b_n += 1;
    thread_yield();
  }
  printf("thread_b: exit after %d\n", b_n);

  current_thread->state = FREE;
  thread_schedule();
}

void 
thread_c(void)
{
  int i;
  printf("thread_c started\n");
  c_started = 1;
  while(a_started == 0 || b_started == 0)
    thread_yield();
  
  for (i = 0; i < 100; i++) {
    printf("thread_c %d\n", i);
    c_n += 1;
    thread_yield();
  }
  printf("thread_c: exit after %d\n", c_n);

  current_thread->state = FREE;
  thread_schedule();
}

int 
main(int argc, char *argv[]) 
{
  a_started = b_started = c_started = 0;
  a_n = b_n = c_n = 0;
  thread_init();
  thread_create(thread_a);
  thread_create(thread_b);
  thread_create(thread_c);
  thread_schedule();
  exit(0);
}
