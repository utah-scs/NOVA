/*
 * MICA implementation in NAAM framework 
 */

#include "naam.h"
#include "table.h"

#define BUCKET_OFFSET(bucket_index) sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * bucket_index)

typedef struct __attribute__((packed)) kv_pair
{
  uint8_t cmd;
  KEY_TYPE key;
  VAL_TYPE val;
  uint8_t status;
} kv_pair;

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx)
{
  uint64_t pid;
  int num_try = 3;
  dma_addr_t addr_from, addr_to;
  CAS_TYPE ret;
  uint64_t offset;
  
  void *pkt = (void *)(long)ctx->data;
  void *pkt_end = (void *)(long)ctx->data_end;

  if (pkt + MAX_PKT_SZ > pkt_end)
    return 1;
    
  // Fetch client request data from packet buf
  kv_pair *kv = (kv_pair *)APP_REGION_PTR(pkt);
    
  uint64_t key = kv->key;
  uint64_t key_hash;
  
  bool version_flag = false;
  uint16_t tag;
  // Serach item vector for the key
  VAL_TYPE user_val = NULL;
  int item_index;
  struct mehcached_bucket *current_bucket;
  struct mehcached_item *item;

  current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair));
  
  for (item_index = 0; item_index < MEHCACHED_ITEMS_PER_BUCKET; item_index++)
  {
    if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) != tag)
      continue;

    // Found, read the item
    offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
    addr_from = DMA_ADDR(1, 1, offset);
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));
    
    // Read the item from the table
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_item)) != RET_DMA_SUCCESS)
    {
      DPrintf("SET %lu: ITEM READ FAILED KEY -> %lu\n", pid, key);
      kv->status = HT_ERR_FATAL;
      goto end;
    }
    
    item = (struct mehcached_item *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair));
    DPrintf("GET %lu: FOUND VAL -> %lu, INDEX -> %d\n", pid, item->val, item_index);

    if (item->key_hash != key_hash)
    {
      DPrintf("GET %lu: HASH FAILED KEY -> %lu\tITEM_H -> %lu\t"
          "KEY_H -> %lu\n", pid, key, item->key_hash, key_hash);
      continue;
    }
    DPrintf("GET %lu: HASH MATCH KEY -> %lu\tITEM_H -> %lu\t"
        "KEY_H -> %lu\n", pid, key, item->key_hash, key_hash);
    user_val = item->val;
    break;
  }

end:
  send_reply(pkt);
  return 0;
}
