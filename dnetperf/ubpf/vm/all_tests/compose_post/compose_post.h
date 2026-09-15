#ifndef __COMPOSE_POST_H__
#define __COMPOSE_POST_H__

#include <inttypes.h>
#include "naam.h"
#include "table.h"

#define MAX_FOLLOWERS 100

#define MAX_TEXT_LEN 140
#define MAX_USER_NAME_LEN 20
#define MAX_MEDIA 5
#define MAX_MEDIA_STR_LEN 10
#define MAX_URL_LEN 10
#define MAX_URLS 5
#define MAX_MENTIONS 5

#define POST_DB_MEMREG_ID 2
#define POST_KV_MEMREG_ID 3
#define USER_TIMELINE_KV_MEMREG_ID 4
#define HOME_TIMELINE_KV_MEMREG_ID 5
#define SOCIAL_GRAPH_KV_MEMREG_ID 6

#define BUCKET_OFFSET(bucket_index) sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * bucket_index)

#define __always_inline inline __attribute__((always_inline))

/*
 * Input to compose_post: (pulled from packet buffer)
 * - req_id: Request ID
 * - user_name:
 * - user_id:
 * - text:
 * - media_ids:
 * - media_types:
 * - post_type:
 */
typedef struct __attribute__((packed)) compose_post_req {
  uint64_t req_id;
  char user_name[MAX_USER_NAME_LEN];
  uint64_t user_id;
  // text field might be modified by compose_post function
  char text[MAX_TEXT_LEN];
  uint8_t num_media;
  uint64_t media_ids[MAX_MEDIA];
  char media_types[MAX_MEDIA][MAX_MEDIA_STR_LEN];
  int post_type;
  // following fields will be filled by compose_post function
  uint64_t created_at;
  uint64_t post_id;
  uint8_t num_urls;
  char urls[MAX_URLS][MAX_URL_LEN];
  uint8_t num_mentions;
  char mentions[MAX_MENTIONS][MAX_USER_NAME_LEN];
} compose_post_req_t;

static int __always_inline mica_set(void *pkt, uint64_t mem_reg, uint64_t pkt_off, uint64_t keyarg, uint64_t valarg) {
    uint64_t key_hash;
    CAS_TYPE ret;
    uint64_t pid = 0;
    int is_success = 1;
    uint64_t key = keyarg;
    uint64_t val = valarg;
    dma_addr_t addr_from, addr_to;
    uint64_t offset;
    bpf_hash(&key, sizeof(key_hash), &key_hash, sizeof(key_hash), HASH_SEED);
    
    uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (NUM_BUCKETS - 1);
    uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);
    
    DPrintf("SET %lu: BEGIN KEY -> %lu VALUE -> %lu BUCKET -> %u\n", pid, key, val, bucket_index);
    
    addr_from = DMA_ADDR(1, mem_reg, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
    
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
    {
      DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
      is_success = 0;
      goto endset;
    }
    
    struct mehcached_bucket *current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt));
   
    DPrintf("SET %lu: BUCKET -> %u VERSION -> %u KEY -> %lu\n", pid, bucket_index, current_bucket->version, key);
    uint32_t cur_version = current_bucket->version;
    DPrintf("SET %lu: START CAS KEY -> %lu\n", pid, key);

    ret = -1;
    for (int i = 0; i < 2; i++)
    {
      ret = DMA_CAS(pkt, addr_from, (cur_version & ~1U), (cur_version | 1U));
      DPrintf("SET %lu: CAS RET -> %u\n", pid, ret);
      
      if (ret == (cur_version & ~1U))
      {
        DPrintf("SET %lu: NOLOOP CAS RET -> %u KEY -> %lu\n", pid, ret, key);
        break;
      }
      
      if (ret > cur_version) {
        DPrintf("SET %lu: LOOP CAS RET -> %u KEY -> %lu\n", pid, ret, key);
        cur_version = ret + 1;
      }
    }
    
    if (ret != (cur_version & ~1U))
    {
      DPrintf("SET %lu: BUCKET -> %u LOCK CAS FAILED KEY -> %lu\n", pid, bucket_index, key);
      is_success = 0;
      goto endset;
    }
    
    DPrintf("SET %lu: BUCKET -> %u LOCK SUCCESSFUL KEY -> %lu\n", pid, bucket_index, key);
    cur_version = ret + 1;
    
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
    {
      DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
      is_success = 0;
      goto endset;
    }
    
    ret = MEHCACHED_ITEMS_PER_BUCKET + 1;
    
    int item_index = 0;
    uint64_t item_vec = 0;
    for (item_index = 0; item_index < MEHCACHED_ITEMS_PER_BUCKET; item_index++)
    {
      if (current_bucket->item_vec[item_index] == 0)
        break;
      if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) == tag) {
        ret = item_index;
        item_vec = current_bucket->item_vec[item_index];
        DPrintf("SET %lu: GOT ITEM VECT %lu\n", pid, item_vec);
        break;
      }
    }
    
    if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1 && item_index == MEHCACHED_ITEMS_PER_BUCKET) {
      DPrintf("SET %lu: BUCKET -> %u FULL KEY -> %lu\n", pid, bucket_index, key);
      
      addr_from = DMA_ADDR(1, mem_reg, BUCKET_OFFSET(bucket_index));
      ret = DMA_CAS(pkt, addr_from, cur_version, cur_version + 1);
      if (ret == cur_version)
      {
        DPrintf("SET %lu: BUCKET -> %u VERSION UPDATE SUCCESSFUL\n", pid, bucket_index);
      } else {
        DPrintf("SET %lu: BUCKET -> %u VERSION UPDATE FAILED\n", pid, bucket_index);
      }
      
      is_success = 0;
      goto endset;
    }
    
    if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1) {
      DPrintf("SET %lu: ALLOCATING SPACE KEY -> %lu\n", pid, key);
      offset = DMA_FAA(pkt, DMA_ADDR(1, mem_reg, 0), sizeof(struct mehcached_item));
      item_vec = MEHCACHED_ITEM_VEC(tag, offset);
      
      *(uint64_t *)((char *)APP_REGION_PTR(pkt)) = item_vec;
      
      addr_from = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
      offset = MEHCACHED_ROUNDUP8(sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index
              + sizeof(current_bucket->version) + sizeof(uint64_t) * item_index);
      addr_to = DMA_ADDR(1, mem_reg, offset);
      DMA_WRITE(pkt, addr_to, addr_from, sizeof(item_vec));
    } else {
      DPrintf("SET %lu: ITEM PRESENT\n", pid);
    }
    
    struct mehcached_item item = {.key_hash = key_hash, .key = key, .val = val};
    
    *(struct mehcached_item *)((char *)APP_REGION_PTR(pkt)) = item;
    
    addr_from = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
    offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * NUM_BUCKETS + MEHCACHED_ITEM_OFFSET(item_vec);
    addr_to = DMA_ADDR(1, mem_reg, offset);
    
    DMA_WRITE(pkt, addr_to, addr_from, sizeof(struct mehcached_item)); 
    
    addr_from = DMA_ADDR(1, mem_reg, BUCKET_OFFSET(bucket_index));
    ret = DMA_CAS(pkt, addr_from, cur_version, cur_version + 1);
    if (ret == cur_version)
    {
      DPrintf("SET %lu: VERSION_UPDATE SUCCESSFUL KEY -> %lu O_VERSION -> %u N_VERSION -> %u\n", pid, key, cur_version, cur_version + 1);
    } else {
      DPrintf("SET %lu: VERSION_UPDATE FAILED OLD VERSION -> %u RET -> %u KEY -> %lu\n", pid, cur_version, ret, key);
      is_success = 0;
      goto endset;
    }
endset:
    return is_success;
}

static int __always_inline mica_get(void *pkt, uint64_t mem_reg, uint64_t pkt_off, uint64_t keyarg, uint64_t *valret) {
    uint64_t key = keyarg;
    uint64_t key_hash;
    dma_addr_t addr_from, addr_to;
    uint64_t offset;
    int is_success = 1;
    
    bpf_hash(&key, sizeof(key), &key_hash, sizeof(key_hash), HASH_SEED);
    uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (NUM_BUCKETS - 1);
    uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);
    DPrintf("GET %lu: BEGIN KEY -> %lu BUCKET -> %u\n", pid, key, bucket_index);
    
    DPrintf("GET %lu: TRYING TO GET UNLOCKED BUCKET KEY -> %lu\n", pid, key);
    
    addr_from = DMA_ADDR(1, mem_reg, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
    
    uint32_t version_counter;
    bool version_flag = false;
    uint32_t prev = 0;
    struct mehcached_bucket *current_bucket;
    
    for (version_counter = 0; version_counter < 2; version_counter++)
    {
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt));
      DPrintf("GET %lu: BUCKET -> %u, VERSION -> %u, KEY -> %lu\n", pid, bucket_index, current_bucket->version, key);
      
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
      is_success = 0;
      goto endget;
    }
    
    version_flag = false;
    VAL_TYPE user_val = 0;
    int item_index;
    struct mehcached_item *item;
    
    for (item_index = 0; item_index < 4; item_index++)
    {
      if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) != tag)
        continue;
      
      offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
      addr_from = DMA_ADDR(1, mem_reg, offset);
      addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
      
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_item)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: ITEM READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      item = (struct mehcached_item *)((char *)APP_REGION_PTR(pkt));
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
      
      offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
      addr_from = DMA_ADDR(1, mem_reg, offset);
      addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
      
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_item)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: ITEM READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      item = (struct mehcached_item *)((char *)APP_REGION_PTR(pkt));
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
      
      offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
      addr_from = DMA_ADDR(1, mem_reg, offset);
      addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
      
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_item)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: ITEM READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      item = (struct mehcached_item *)((char *)APP_REGION_PTR(pkt));
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
      
      offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
      addr_from = DMA_ADDR(1, mem_reg, offset);
      addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
      
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_item)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: ITEM READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      item = (struct mehcached_item *)((char *)APP_REGION_PTR(pkt));
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
      is_success = 0;
      goto endget;
    }
    else
      DPrintf("GET %lu: SUCCESSFUL WITHOUT VERSION CHECK KEY -> %lu, VAL -> %lu\n", pid, key, user_val);
   
    addr_from = DMA_ADDR(1, mem_reg, BUCKET_OFFSET(bucket_index));
    addr_to = DMA_ADDR(1, 0, APP_REGION_OFFSET(pkt) + pkt_off);
    
    for (version_counter = 0; version_counter < 2; version_counter++)
    {
      if (DMA_READ(pkt, addr_to, addr_from, sizeof(struct mehcached_bucket)) != RET_DMA_SUCCESS)
      {
        DPrintf("SET %lu: BUCKET READ FAILED KEY -> %lu\n", pid, key);
        is_success = 0;
        goto endget;
      }
      
      current_bucket = (struct mehcached_bucket *)((char *)APP_REGION_PTR(pkt));
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
      is_success = 0;
      goto endget;
    }
    
    DPrintf("GET %lu: SUCCESSFUL KEY -> %lu VALUE -> %lu\n", pid, key, user_val);
    *valret = user_val;

endget:
    return is_success;
}
#endif
