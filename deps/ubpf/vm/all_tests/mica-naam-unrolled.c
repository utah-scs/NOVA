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
    
  if (NAAM_DEBUG)
    pid = bpf_rand();

  if (kv->cmd == REQT_SET)
  {
    
    uint64_t key_hash;
    bpf_hash(&kv->key, sizeof(key_hash), &key_hash, sizeof(key_hash), HASH_SEED);

    // Calculate the bucket index and tag from the key hash
    uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (NUM_BUCKETS - 1);
    uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);
    
    DPrintf("SET %lu: BEGIN KEY -> %lu VALUE -> %lu BUCKET -> %u\n", pid, kv->key, kv->val, bucket_index);

    // Fetch bucket from memory region
    // and write it to the packet buffer application region
    addr_from = DMA_ADDR(1, 1, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));

    // Read the bucket from the table
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
    {
      DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, kv->key);
      kv->status = HT_ERR_FATAL;
      goto end;
    }

    // Fetched bucket should be in the application region of the packet buffer
    struct mehcached_bucket *current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair));

    DPrintf("SET %lu: BUCKET -> %u VERSION -> %u KEY -> %lu\n", pid, bucket_index, current_bucket->version, kv->key);
    uint32_t cur_version = current_bucket->version;
    DPrintf("SET %lu: START CAS KEY -> %lu\n", pid, kv->key);

    // Increment the version number to start writing value
    ret = -1; 
    for (int i = 0; i < 2; i++)
    {
      // Try to replace version number x with x + 1
      // where x is even, and x + 1 is odd
      //
      // If x is odd already, it is rounded down to the previous even number
      // and then try to replace it with x + 1
      ret = DMA_CAS(pkt, addr_from, (cur_version & ~1U), (cur_version | 1U));

      DPrintf("SET %lu: CAS RET -> %u\n", pid, ret);

      // If x is returned from CAS means the bucket is free and CAS was successful
      if (ret == (cur_version & ~1U))
      {
        DPrintf("SET %lu: NOLOOP CAS RET -> %u KEY -> %lu\n", pid, ret, kv->key);
        break;
      }
      
      // Another process has claimed the bucket, attempt to CAS the version
      // that comes next
      if (ret > cur_version){
        DPrintf("SET %lu: LOOP CAS RET -> %u KEY -> %lu\n", pid, ret, kv->key);
        cur_version = ret + 1;
      }
    }

    if (ret != (cur_version & ~1U))
    {
      DPrintf("SET %lu: BUCKET -> %u LOCK CAS FAILED KEY -> %lu\n", pid, bucket_index, kv->key);
      kv->status = HT_ERR_LOCKED;
      goto end;
    }

    // bucket lock successful, increment version after writing
    DPrintf("SET %lu: BUCKET -> %u LOCK SUCCESSFUL KEY -> %lu\n", pid, bucket_index, kv->key);
    cur_version = ret + 1;
    
    //Re-read bucket to ensure the latest version is held.
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
    {
      DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, kv->key);
      kv->status = HT_ERR_FATAL;
      goto end;
    }

    //Search item vector for user key
    ret = MEHCACHED_ITEMS_PER_BUCKET + 1;

    int item_index = 0;
    uint64_t item_vec = 0;
    for (item_index = 0; item_index < MEHCACHED_ITEMS_PER_BUCKET; item_index++)
    {
      //insertions are always in order, removal of any kind is not supported
      //so if a blank entry is foundbefore a match the key should just be inserted
      if (current_bucket->item_vec[item_index] == 0)
        break;
      if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) == tag){
        ret = item_index;
        item_vec = current_bucket->item_vec[item_index];
        DPrintf("SET %lu: GOT ITEM VECT %lu\n", pid, item_vec);
        break;
      }
    }


    // Bucket full
    if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1 && item_index == MEHCACHED_ITEMS_PER_BUCKET){
      DPrintf("SET %lu: BUCKET -> %u FULL KEY -> %lu\n", pid, bucket_index, kv->key);
      
      // Unlock bucket
      addr_from = DMA_ADDR(1, 1, BUCKET_OFFSET(bucket_index));
      ret = DMA_CAS(pkt, addr_from, cur_version, cur_version + 1);
      if (ret == cur_version)
      {
        DPrintf("SET %lu: BUCKET -> %u VERSION UPDATE SUCCESSFUL\n", pid, bucket_index);
      } else {
        DPrintf("SET %lu: BUCKET -> %u VERSION UPDATE FAILED\n", pid, bucket_index);
      }
      
      kv->status = HT_ERR_FULL;
      goto end;
    }

    
    // Allocate space for an item if needed
    if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1){
      DPrintf("SET %lu: ALLOCATING SPACE KEY -> %lu\n", pid, kv->key);
      offset = DMA_FAA(pkt, DMA_ADDR(1, 1, 0), sizeof(struct mehcached_item));
      item_vec = MEHCACHED_ITEM_VEC(tag, offset);

      
      // Update memory region bucket with new item vector

      // Write the item vector to the packet buffer application region
      *(uint64_t *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair)) = item_vec;
      
      addr_from = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));
      
      offset = MEHCACHED_ROUNDUP8(sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index
              + sizeof(current_bucket->version) + sizeof(uint64_t) * item_index);
      
      addr_to = DMA_ADDR(1, 1, offset);
      
      DMA_WRITE(pkt, addr_to, addr_from, sizeof(item_vec));
    } else {
      DPrintf("SET %lu: ITEM PRESENT\n", pid);
    }

    struct mehcached_item item = {.key_hash = key_hash, .key = kv->key, .val = kv->val};

    // Write actual KV item
    *(struct mehcached_item *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair)) = item;
      
    addr_from = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));
      
    offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * NUM_BUCKETS + MEHCACHED_ITEM_OFFSET(item_vec);
    addr_to = DMA_ADDR(1, 1, offset);

    DMA_WRITE(pkt, addr_to, addr_from, sizeof(struct mehcached_item));

    // Unlock the bucket by increasing version number
    // and result in an even number
    addr_from = DMA_ADDR(1, 1, BUCKET_OFFSET(bucket_index));
    ret = DMA_CAS(pkt, addr_from, cur_version, cur_version + 1);
    if (ret == cur_version)
    {
      DPrintf("SET %lu: VERSION_UPDATE SUCCESSFUL KEY -> %lu O_VERSION -> %u N_VERSION -> %u\n", pid, kv->key, cur_version, cur_version + 1);
    } else {
      DPrintf("SET %lu: VERSION_UPDATE FAILED OLD VERSION -> %u RET -> %u KEY -> %lu\n", pid, cur_version, ret, kv->key);
      kv->status = HT_ERR_VERSION_UPDATE;
      goto end;
    }
    
    kv->status = HT_SUCCESS;
  }
  else // READ
  {
    uint64_t key = kv->key;
    uint64_t key_hash;
    
    bpf_hash(&key, sizeof(key), &key_hash, sizeof(key_hash), HASH_SEED);
    uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (NUM_BUCKETS - 1);
    uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);
    DPrintf("GET %lu: BEGIN KEY -> %lu BUCKET -> %u\n", pid, key, bucket_index);
    
    // Try to get a even version number to get unlocked bucket
    DPrintf("GET %lu: TRYING TO GET UNLOCKED BUCKET KEY -> %lu\n", pid, key);
    
    // Fetch bucket from memory region
    // and write it to the packet buffer application region
    addr_from = DMA_ADDR(1, 1, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));

    uint32_t version_counter;
    bool version_flag = false;
    uint32_t prev = 0;
    struct mehcached_bucket *current_bucket;
    
    for (version_counter = 0; version_counter < 2; version_counter++)
    {
      // Read the bucket from the table
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
        kv->status = HT_ERR_FATAL;
        goto end;
      }

      current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair));
      DPrintf("GET %lu: BUCKET -> %u, VERSION -> %u, KEY -> %lu\n", pid, bucket_index, current_bucket->version, key);

      // If current version is not even then continue
      prev = current_bucket->version;
      if ((prev & 1U) != 0U)
      {
        continue;
      }
      else
      {
        version_flag = true;
        break;
      }
    }

    if (version_flag == false)
    {
      DPrintf("GET %lu: BUCKET -> %u VERSION FAILED KEY -> %lu\n", pid, bucket_index, key);
      kv->status = HT_ERR_LOCKED;
      goto end;
    }
    
    version_flag = false;
    // Serach item vector for the key
    VAL_TYPE user_val = NULL;
    int item_index;
    struct mehcached_item *item;
    
    /*
     * TODO:
     *
     * This loop is unrolled. Otherwise the verifier complains with message:
     *
     * 105: Lower bound must be at least meta_offset (valid_access(r1.offset, width=8) for read)
     * 105: Upper bound must be at most packet_size (valid_access(r1.offset, width=8) for read)
     *
     * Possible reason: Without loop unrolling the compiler generates code pattern like this:
     *
     *      r8 = 0
     * label:
     *      ...
     *      r1 += r8  // r1 points to offset in the packet buffer
     *      ...
     *      goto label
     *
     * May be the verifier is failing to ensure packet bound after adding r8 with r1
     */
    for (item_index = 0; item_index < 4; item_index++)
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
    
    for (item_index = 4; item_index < 8; item_index++)
    {
      if (user_val != NULL) 
        break;

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
    
    for (item_index = 8; item_index < 12; item_index++)
    {
      if (user_val != NULL) 
        break;

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
    
    for (item_index = 12; item_index < 16; item_index++)
    {
      if (user_val != NULL) 
        break;

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

    if(user_val == NULL) {
      DPrintf("GET %lu: ITEM NOT FOUND KEY -> %lu\n", pid, key);
      kv->status = HT_ERR_KEY_NOT_FOUND;
      goto end;
    }
    else
      DPrintf("GET %lu: SUCCESSFUL WITHOUT VERSION CHECK KEY -> %lu, VAL -> %lu\n", pid, key, user_val);
    
    // Fetch bucket from memory region
    // and write it to the packet buffer application region
    addr_from = DMA_ADDR(1, 1, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + sizeof(kv_pair));
    
    for (version_counter = 0; version_counter < 2; version_counter++)
    {
      // Read the bucket from the table
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
        kv->status = HT_ERR_FATAL;
        goto end;
      }

      current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt) + sizeof(kv_pair));
      DPrintf("GET %lu: BUCKET -> %u, VERSION -> %u, KEY -> %lu\n", pid, bucket_index, current_bucket->version, key);

      if ((current_bucket->version) != prev)
      {
        break;
      }
      else
      {
        version_flag = true;
        break;
      }
    }

    if (version_flag == false)
    {
      DPrintf("GET %lu: VERSION_CHECK FAILED KEY -> %lu\n", pid, key);
      kv->status = HT_ERR_VERSION_UPDATE;
      goto end;
    }
    
    DPrintf("GET %lu: SUCCESSFUL KEY -> %lu VALUE -> %lu\n", pid, key, user_val);
    kv->val = user_val;
    kv->status = HT_SUCCESS;
  }

end:
  send_reply(pkt);
  return 0;
}
