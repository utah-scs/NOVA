// Standalone MICA (mehcached) key-value hashtable module.
//
// Executes the same GET/SET logic as experiments/MICA_MULTI/mica-naam.c,
// but directly on local memory instead of through the UBPF + DMA pipeline.
//
// The table is shared by all MICA instances in the process, so a multicore
// pipeline creates one MICA() per worker and they all operate on the same
// table. Concurrency follows MICA: writers lock a bucket by CAS'ing its
// version from even to odd, readers are optimistic and retry if the version
// changed while they were reading.

#include "mica.h"

#include <sys/mman.h>

#include <algorithm>

#include "../../deps/util/murmur.c"

#define MICA_TAG_MASK (((uint64_t)1 << 16) - 1)
#define MICA_TAG(item_vec) ((item_vec) >> 48)
#define MICA_ITEM_IDX_MASK (((uint64_t)1 << 48) - 1)
#define MICA_ITEM_IDX(item_vec) ((item_vec) & MICA_ITEM_IDX_MASK)
#define MICA_ITEM_VEC(tag, idx) (((uint64_t)(tag) << 48) | (uint64_t)(idx))

// Bounded retries so a worker never spins forever on a bucket
static const int kMaxLockTries = 1024;
static const int kMaxReadTries = 1024;

typedef struct __attribute__((packed)) mica_kv {
  uint8_t cmd;
  KEY_TYPE key;
  VAL_TYPE val;
  uint8_t status;
} mica_kv_t;

struct mica_table MICA::table_ = {};
std::mutex MICA::table_lock_;

static inline void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

static inline uint64_t roundup_pow2(uint64_t v) {
  if (v <= 1)
    return 1;
  return (uint64_t)1 << (64 - __builtin_clzll(v - 1));
}

static inline uint64_t mica_hash(uint64_t key) {
  // MurmurHash3_x86_128 writes 128 bits, the NAAM version uses the low 64
  uint64_t h[2];
  MurmurHash3_x86_128(&key, sizeof(key), HASH_SEED, h);
  return h[0];
}

static void *alloc_region(size_t size) {
  // Prefer hugepages, fall back to regular pages
  void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                   -1, 0);
  if (mem != MAP_FAILED)
    return mem;

  mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  if (mem == MAP_FAILED)
    return NULL;
  return mem;
}

int MICA::SetupTable(uint64_t num_keys) {
  // Same bucket sizing as the DMA module's hashtable
  uint64_t num_buckets = roundup_pow2(std::max<uint64_t>(num_keys / MICA_ITEMS_PER_BUCKET, 1));

  // The client draws keys from [0, roundup_pow2(num_keys)), so leave room
  // for SETs that insert keys beyond the pre-populated range. Index 0 is
  // reserved so that an item vector is never 0.
  uint64_t num_items = roundup_pow2(num_keys) + 1;

  size_t buckets_size = num_buckets * sizeof(struct mica_bucket);
  size_t items_size = num_items * sizeof(struct mica_item);
  size_t mem_size = buckets_size + items_size;
  // Round up to 1GB so that the hugepage mapping succeeds
  mem_size = ((mem_size + (1UL << 30) - 1) >> 30) << 30;

  void *mem = alloc_region(mem_size);
  if (!mem)
    return -1;

  table_.buckets = reinterpret_cast<struct mica_bucket *>(mem);
  table_.items = reinterpret_cast<struct mica_item *>((char *)mem + buckets_size);
  table_.num_keys = num_keys;
  table_.num_buckets = num_buckets;
  table_.num_items = num_items;
  table_.next_item = 1;
  table_.mem_size = mem_size;

  LOG(INFO) << "MICA: keys " << num_keys << ", buckets " << num_buckets
            << ", item capacity " << num_items - 1 << ", memory "
            << mem_size << " bytes";

  return 0;
}

void MICA::Populate() {
  uint64_t failed = 0;

  // Pre-populate keys [0, num_keys) with value = key + 1, which is what
  // the client verifies on a successful GET. nseq = 0 means never written.
  for (uint64_t key = 0; key < table_.num_keys; key++) {
    if (Set(key, key + 1, 0) != HT_SUCCESS)
      failed++;
  }

  if (failed)
    LOG(WARNING) << "MICA: " << failed << " keys did not fit (bucket full)";
}

CommandResponse MICA::Init(const bess::pb::MICAArg &arg) {
  uint64_t num_keys = arg.num_keys() > 0 ? arg.num_keys() : NUM_KEYS;

  std::lock_guard<std::mutex> guard(table_lock_);

  if (table_.refcnt > 0) {
    if (table_.num_keys != num_keys)
      return CommandFailure(EINVAL,
                            "MICA table already exists with num_keys=%lu",
                            (unsigned long)table_.num_keys);
    table_.refcnt++;
    return CommandSuccess();
  }

  if (SetupTable(num_keys))
    return CommandFailure(ENOMEM, "Failed to allocate MICA table");

  Populate();
  table_.refcnt = 1;

  return CommandSuccess();
}

void MICA::DeInit() {
  std::lock_guard<std::mutex> guard(table_lock_);

  if (table_.refcnt == 0 || --table_.refcnt > 0)
    return;

  munmap(table_.buckets, table_.mem_size);
  table_ = {};
}

uint8_t MICA::Set(uint64_t key, uint64_t val, uint64_t nseq) {
  uint64_t key_hash = mica_hash(key);
  uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (table_.num_buckets - 1);
  uint16_t tag = (uint16_t)(key_hash & MICA_TAG_MASK);
  struct mica_bucket *bucket = &table_.buckets[bucket_index];

  // Lock the bucket: version x (even) -> x + 1 (odd)
  uint32_t version = 0;
  bool locked = false;
  for (int i = 0; i < kMaxLockTries; i++) {
    version = __atomic_load_n(&bucket->version, __ATOMIC_RELAXED);
    if ((version & 1U) == 0U &&
        __atomic_compare_exchange_n(&bucket->version, &version, version + 1,
                                    false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      locked = true;
      break;
    }
    cpu_relax();
  }

  if (!locked)
    return HT_ERR_LOCKED;

  uint8_t status = HT_SUCCESS;
  struct mica_item *item = nullptr;
  int free_index = -1;

  // Insertions are in order and removal is not supported, so the first
  // empty slot marks the end of the bucket
  for (int i = 0; i < MICA_ITEMS_PER_BUCKET; i++) {
    uint64_t item_vec = bucket->item_vec[i];
    if (item_vec == 0) {
      free_index = i;
      break;
    }
    if (MICA_TAG(item_vec) == tag &&
        table_.items[MICA_ITEM_IDX(item_vec)].key == key) {
      item = &table_.items[MICA_ITEM_IDX(item_vec)];
      break;
    }
  }

  if (item) {
    // Dedup: drop writes whose nseq was already applied
    if (item->nseq >= nseq && nseq != 0) {
      status = HT_ERR_DUPLICATE;
      goto unlock;
    }
    __atomic_store_n(&item->val, val, __ATOMIC_RELAXED);
    item->nseq = nseq;
  } else {
    if (free_index < 0) {
      status = HT_ERR_FULL;
      goto unlock;
    }

    uint64_t idx = __atomic_fetch_add(&table_.next_item, 1, __ATOMIC_RELAXED);
    if (idx >= table_.num_items) {
      status = HT_ERR_FULL;
      goto unlock;
    }

    item = &table_.items[idx];
    item->key_hash = key_hash;
    item->key = key;
    item->val = val;
    item->nseq = nseq;

    // Publish the item after it is fully written
    __atomic_store_n(&bucket->item_vec[free_index], MICA_ITEM_VEC(tag, idx),
                     __ATOMIC_RELEASE);
  }

unlock:
  // Unlock the bucket: x + 1 (odd) -> x + 2 (even)
  __atomic_store_n(&bucket->version, version + 2, __ATOMIC_RELEASE);
  return status;
}

uint8_t MICA::Get(uint64_t key, uint64_t *val) {
  uint64_t key_hash = mica_hash(key);
  uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (table_.num_buckets - 1);
  uint16_t tag = (uint16_t)(key_hash & MICA_TAG_MASK);
  struct mica_bucket *bucket = &table_.buckets[bucket_index];
  uint8_t status = HT_ERR_LOCKED;

  for (int t = 0; t < kMaxReadTries; t++) {
    uint32_t v1 = __atomic_load_n(&bucket->version, __ATOMIC_ACQUIRE);
    if ((v1 & 1U) != 0U) {
      status = HT_ERR_LOCKED;
      cpu_relax();
      continue;
    }

    bool found = false;
    uint64_t found_val = 0;
    for (int i = 0; i < MICA_ITEMS_PER_BUCKET; i++) {
      uint64_t item_vec = __atomic_load_n(&bucket->item_vec[i], __ATOMIC_ACQUIRE);
      if (item_vec == 0)
        break;
      if (MICA_TAG(item_vec) != tag)
        continue;

      struct mica_item *item = &table_.items[MICA_ITEM_IDX(item_vec)];
      if (__atomic_load_n(&item->key, __ATOMIC_RELAXED) != key)
        continue;

      found_val = __atomic_load_n(&item->val, __ATOMIC_RELAXED);
      found = true;
      break;
    }

    // Make sure a writer did not touch the bucket while we were reading
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint32_t v2 = __atomic_load_n(&bucket->version, __ATOMIC_RELAXED);
    if (v1 != v2) {
      status = HT_ERR_VERSION_UPDATE;
      cpu_relax();
      continue;
    }

    if (!found)
      return HT_ERR_KEY_NOT_FOUND;

    *val = found_val;
    return HT_SUCCESS;
  }

  return status;
}

void MICA::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  for (int i = 0; i < cnt; i++) {
    bess::Packet *pkt = batch->pkts()[i];

    // sizeof(req_pkt_t) includes tail padding, the payload starts at data
    if ((size_t)pkt->data_len() < offsetof(req_pkt_t, data) + sizeof(mica_kv_t)) {
      DropPacket(ctx, pkt);
      continue;
    }

    req_pkt_t *req = pkt->head_data<req_pkt_t *>();
    mica_kv_t *kv = reinterpret_cast<mica_kv_t *>(req->data);

    if (kv->cmd == REQT_SET) {
      kv->status = Set(kv->key, kv->val, req->nseq);
    } else {
      uint64_t val;
      kv->status = Get(kv->key, &val);
      if (kv->status == HT_SUCCESS)
        kv->val = val;
    }

    // Turn the request into a reply (same as send_reply() in naam.h)
    unsigned char eth_tmp[ETH_ALEN];
    memcpy(eth_tmp, req->eth.h_source, ETH_ALEN);
    memcpy(req->eth.h_source, req->eth.h_dest, ETH_ALEN);
    memcpy(req->eth.h_dest, eth_tmp, ETH_ALEN);

    uint32_t ip_tmp = req->ip.saddr;
    req->ip.saddr = req->ip.daddr;
    req->ip.daddr = ip_tmp;

    uint16_t udp_tmp = req->udp.source;
    req->udp.source = req->udp.dest;
    req->udp.dest = udp_tmp;

    EmitPacket(ctx, pkt, 0);
  }
}

ADD_MODULE(MICA, "mica", "standalone MICA key-value hashtable")
