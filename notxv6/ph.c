#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#define NBUCKET 5  // 哈希桶个数：越大越能分散冲突（实验固定为5）
#define NKEYS 100000// 总插入键数量
// 链表节点：每个桶是一个单链表，节点保存 key/value
struct entry {
  int key;
  int value;
  struct entry *next;
};

struct entry *table[NBUCKET]; // 哈希表：table[i] 指向第 i 个桶的链表表头
int keys[NKEYS]; // 随机键集合（预生成，供 put/get 使用）
int nthread = 1;

// 声明锁数组
pthread_mutex_t locks[NBUCKET];  // 每个桶一个锁
// 返回当前时间（秒，double），用于简单性能计时
double now() {
  struct timeval tv;
  gettimeofday(&tv, 0);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}
// 在 *p 链表头前插入一个新节点（头插法）
static void insert(int key, int value, struct entry **p, struct entry *n) {
  struct entry *e = malloc(sizeof(struct entry));
  e->key = key;
  e->value = value;
  e->next = n;
  *p = e;
}
// 插入或更新键值对：并发正确性的关键路径
static void put(int key, int value) {
  int i = key % NBUCKET; // 选择落在哪个桶（简单取模哈希）

  // 先无锁遍历：判断 key 是否已经存在
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }

  if (e) {// 已存在：更新 value（实验关注“键是否丢失”，非并发更新同一 key 的最终值）
    // update the existing key
    e->value = value;
  } else {
    //不存在：需要在桶表头插入新节点
    pthread_mutex_lock(&locks[i]);  // 对第i个桶加锁
    insert(key, value, &table[i], table[i]);
    pthread_mutex_unlock(&locks[i]);  // 解锁
  }
}

static struct entry* get(int key) {
  int i = key % NBUCKET;
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }
  return e;
}

static void *put_thread(void *xa) {
  int n = (int) (long) xa;  // thread number
  int b = NKEYS / nthread;

  for (int i = 0; i < b; i++) {
    put(keys[b * n + i], n);
  }

  return NULL;
}

static void *get_thread(void *xa) {
  int n = (int) (long) xa;  // thread number
  int missing = 0;

  for (int i = 0; i < NKEYS; i++) {
    struct entry *e = get(keys[i]);
    if (e == 0) missing++;
  }
  printf("%d: %d keys missing\n", n, missing);
  return NULL;
}

int main(int argc, char *argv[]) {
  pthread_t *tha;
  void *value;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s nthreads\n", argv[0]);
    exit(-1);
  }

  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);
  assert(NKEYS % nthread == 0);

  // 初始化所有锁
  for (int i = 0; i < NBUCKET; i++) {
    pthread_mutex_init(&locks[i], NULL);
  }

  // 填充键数组
  for (int i = 0; i < NKEYS; i++) {
    keys[i] = random();
  }

  //
  // First the puts
  //
  t0 = now();
  for (int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, put_thread, (void *) (long) i) == 0);
  }
  for (int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d puts, %.3f seconds, %.0f puts/second\n",
         NKEYS, t1 - t0, NKEYS / (t1 - t0));

  //
  // Now the gets
  //
  t0 = now();
  for (int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, get_thread, (void *) (long) i) == 0);
  }
  for (int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d gets, %.3f seconds, %.0f gets/second\n",
         NKEYS * nthread, t1 - t0, (NKEYS * nthread) / (t1 - t0));

  // 销毁所有锁
  for (int i = 0; i < NBUCKET; i++) {
    pthread_mutex_destroy(&locks[i]);
  }

  return 0;
}

