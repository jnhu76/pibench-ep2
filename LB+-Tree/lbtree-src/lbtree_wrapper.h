#include <omp.h>
#include "tree_api.hpp"
#include "lbtree.h"
#include <thread>
#include <sys/types.h>

// #define DEBUG_MSG

auto num_bulkloaded = 1ll;
float bfill = 1.0;
class lbtree_wrapper : public tree_api
{
public:
  lbtree_wrapper(void *nvm_addr, bool recover);
  virtual ~lbtree_wrapper();

  virtual bool find(const char *key, size_t key_sz, char *value_out) override;
  virtual bool insert(const char *key, size_t key_sz, const char *value, size_t value_sz) override;
  virtual bool update(const char *key, size_t key_sz, const char *value, size_t value_sz) override;
  virtual bool remove(const char *key, size_t key_sz) override;
  virtual int scan(const char *key, size_t key_sz, int scan_sz, char *&values_out) override;

private:
  tree *lbt;
};
extern tree *the_treep;
extern thread_local int worker_id;

#ifdef VAR_KEY
  thread_local char k[128];
  extern PMEMobjpool * pop_;

  #ifndef PMEM
    thread_local char* key_arr = new char[813600000]; // 100M key + 1.7M for 10 seconds
    thread_local char* cur_addr = key_arr;
    thread_local char* end_addr = key_arr + 813600000;
    thread_local bool init = false;
  #endif
#endif

/**
 * LB+Tree divides memory space so each thread has its own range.
 * Threads cannot share this because there is no concurrency control.
 * This struct should be declared as thread_local inside a block scope.
 * It cannot be namespace scope because initialization of "worker_id"
 * and the helper is unsequenced and leads to undefined behaviour.
 */
struct ThreadHelper
{
  const char *str;
  int id_ = -69;
  ThreadHelper(const char *s) : str{s}
  {
    if (worker_id < 0)
      id_ = omp_get_thread_num();
    else
      id_ = worker_id;
    worker_id = id_;
  #if defined(VAR_KEY) && !defined(PMEM)
    if (!init)
    {
      memset(key_arr, 0, 813600000);
      init = true;
    }
  #endif
    // printf("constructor worker_id: %d (%s)\n", id_, str);
  }
  ~ThreadHelper()
  {
    // printf("destructor worker_id: %d (%s)\n", id_, str);
  }
};

static const constexpr auto FIND = "find";
static const constexpr auto INSERT = "insert";
static const constexpr auto UPDATE = "update";
static const constexpr auto REMOVE = "remove";
static const constexpr auto SCAN = "scan";

lbtree_wrapper::lbtree_wrapper(void *nvm_addr, bool recover)
{
#ifdef NVMFLUSH_STAT
  NVMFLUSH_STAT_init();
#endif
  if (const char *env_p = std::getenv("BULKLOAD"))
  {
    num_bulkloaded = atoll(env_p);
  }
  if (const char *env_p = std::getenv("BFILL"))
  {
    bfill = atof(env_p);
  }
  auto delbulk = false;
  if (const char *env_p = std::getenv("DELETEBULK"))
  {
    delbulk = true;
  }
  lbt = initTree(nvm_addr, false);
  the_treep = lbt;
  simpleKeyInput input(2, 42069, 1);
  auto worker_num = worker_thread_num;
  worker_thread_num = 1;
  auto root = lbt->bulkload(num_bulkloaded, &input, bfill);
  worker_thread_num = worker_num;
  printf("%lld keys bulkloaded (deleted? %d), root is %d, bfill %f. ", num_bulkloaded, delbulk, root, bfill);
#ifndef VAR_KEY
  key_type start, end;
  lbt->check(&start, &end);
  for (auto i = start; i < end && delbulk; i++)
    lbt->del(i);
  printf("lbt->check() start: %lld, end: %lld\n", start, end);
#endif
}

lbtree_wrapper::~lbtree_wrapper()
{
#ifdef NVMFLUSH_STAT
  NVMFLUSH_STAT_print();
#endif
}

inline key_type PBkeyToLB(const char *key, size_t size = 8)
{
  return *reinterpret_cast<const key_type *>(key);
}

inline void *PBvalToLB(const char *value, size_t size = 8)
{
  return reinterpret_cast<void *>(*reinterpret_cast<const int64_t *>(value));
}

bool lbtree_wrapper::find(const char *key, size_t key_sz, char *value_out)
{
  thread_local ThreadHelper t{FIND};
  void *p;
  int pos = -1;
#ifdef VAR_KEY
  memcpy(k, key, key_size_);
  p = lbt->lookup((key_type)k, &pos);
#else
  p = lbt->lookup(PBkeyToLB(key), &pos);
#endif
  if (pos >= 0)
  {
    void *recptr = lbt->get_recptr(p, pos);
    // In reality a persistent pointer should be stored in the tree
    // but we don't so take the address of recptr instead.
    memcpy(value_out, &recptr, ITEM_SIZE);
    return true;
  }
#ifdef DEBUG_MSG
  printf("Search key not found!\n");
#endif
  return false;
}

bool lbtree_wrapper::insert(const char *key, size_t key_sz, const char *value, size_t value_sz)
{
  thread_local ThreadHelper t{INSERT};
#ifdef VAR_KEY // key size > 8
  #ifdef PMEM
    PMEMoid dst;
    pmemobj_zalloc(pop_, &dst, key_size_, TOID_TYPE_NUM(char));
    char* new_k = (char*)pmemobj_direct(dst);
    memcpy(new_k, key, key_size_);
    key_type k = (key_type) new_k;
  #else
    // char* new_k = new char[key_size_];
    if (cur_addr >= end_addr)
    {
      printf("insufficient Memory for keys!\n");
      exit(1);
    }
    memcpy(cur_addr, key, key_size_);
    key_type k = (key_type) cur_addr;
    cur_addr += key_size_;
  #endif
#else
  auto k = PBkeyToLB(key);
#endif
  lbt->insert(k, PBvalToLB(value));
  return true;
}

bool lbtree_wrapper::update(const char *key, size_t key_sz, const char *value, size_t value_sz)
{
  thread_local ThreadHelper t{UPDATE};
  //FIXME
  // Try to find the record first.
  bleaf *p;
  int pos = -1;
  auto k = PBkeyToLB(key);
  IdxEntry ent;
Retry:
  p = (bleaf *)lbt->lookup(k, &pos);
  if (!p || pos < 0)
  {
#ifdef DEBUG_MSG
    printf("Update key not found!\n");
#endif
    return false;
  }
  // volatile long long sum;
  // void *recptr;
  // {
  //   Again:
  //   // 1. RTM begin
  //   if (_xbegin() != _XBEGIN_STARTED)
  //   {
  //     // sum = 0;
  //     // for (int i=(rdtsc() % 1024); i>0; i--) sum += i;
  //     std::this_thread::sleep_for(std::chrono::nanoseconds(1));
  //     goto Again;
  //   }
  //   if (p->lock)
  //   {
  //     _xabort(4);
  //     std::this_thread::sleep_for(std::chrono::nanoseconds(1));
  //     goto Again;
  //   }
  //   p->lock = 1;
  //   _xend();
  // }
  // ent = p->ent[pos];
  // if (p->lock || ent.k != k || !__sync_bool_compare_and_swap((uint64_t*)&(p->ent[pos].ch), *(uint64_t*)&(ent.ch), *(uint64_t*)value))
  //   goto Retry;
  void *recptr = lbt->get_recptr(p, pos);
  memcpy(&recptr, value, ITEM_SIZE);
#ifdef NVMPOOL_REAL
  clwb(p);
  sfence();
#endif
  // p->lock = 0;
  return true;
}

bool lbtree_wrapper::remove(const char *key, size_t key_sz)
{
  thread_local ThreadHelper t{REMOVE};
  //FIXME
  lbt->del(PBkeyToLB(key));
  // Check whether the record is indeed removed.
  // void *p;
  // int pos;
  // p = lbt->lookup(*reinterpret_cast<key_type*>(const_cast<char*>(key)), &pos);
  // if (pos >= 0) {
  //   return false;
  // }
  return true;
}

int lbtree_wrapper::scan(const char *key, size_t key_sz, int scan_sz, char *&values_out)
{
  thread_local ThreadHelper t{SCAN};
  constexpr size_t ONE_MB = 1ULL << 20;
  static thread_local char results[ONE_MB];
  // //FIXME
  values_out = results;
  int scanned = lbt->rangeScan(PBkeyToLB(key), scan_sz, results); // range_scan_by_size
#ifdef DEBUG_MSG
  if (scanned != 100)
    printf("%d records scanned\n", scanned);
#endif
  return scanned;
}