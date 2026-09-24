#ifndef BESS_MODULES_MICA_H_
#define BESS_MODULES_MICA_H_

#include <mutex>

#include "../module.h"
#include "../pb/module_msg.pb.h"

extern "C" {
  #include <naam.h>
}

#define MICA_ITEMS_PER_BUCKET 15

// Same bucket/item layout as the NAAM MICA implementation
// (experiments/MICA_LOCAL/mica/table.h), single allocator.
struct mica_bucket {
  uint32_t version;  // even: unlocked, odd: locked by a writer
  uint64_t item_vec[MICA_ITEMS_PER_BUCKET];  // 16 bit tag | 48 bit item index
};

static_assert(sizeof(struct mica_bucket) == BUCKET_SIZE,
              "sizeof(struct mica_bucket) should be BUCKET_SIZE bytes");

struct mica_item {
  uint64_t key_hash;
  KEY_TYPE key;
  VAL_TYPE val;
  uint64_t nseq;  // nseq of last applied write; 0 = never written
};

// Table shared by every MICA module instance (one instance per worker)
struct mica_table {
  struct mica_bucket *buckets;
  struct mica_item *items;
  uint64_t num_keys;
  uint32_t num_buckets;
  uint64_t num_items;       // capacity of the item pool
  uint64_t next_item;       // allocation cursor, index 0 is reserved
  size_t mem_size;
  int refcnt;
};

class MICA final : public Module {
 public:
  MICA() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::MICAArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  static int SetupTable(uint64_t num_keys);
  static void Populate();

  static uint8_t Set(uint64_t key, uint64_t val, uint64_t nseq);
  static uint8_t Get(uint64_t key, uint64_t *val);

  static struct mica_table table_;
  static std::mutex table_lock_;
};

#endif  // BESS_MODULES_MICA_H_
