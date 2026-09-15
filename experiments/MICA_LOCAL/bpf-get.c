/*
 * Simple set program
 * to experiment with dma write
 */

#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include "naam.h"
#include "../MICA_LOCAL/mica/table.h"
//#include "../../deps/util/murmur.c"
// TODO(stutsman) Why does offsetof not work? It seems to generate a bad
// relocation type 10 due to compiling in a type of section that the ubpf
// loader doesn't support. This seems to work despite the UB.
#define off(type, field) (uintptr_t) & (((type *)0)->field)

#define SET_READ(sorc, dest)  req->type = DMA_READ_OP;  src->mem_id = sorc; dst->mem_id = dest; dma_req.op = DMA_READ_OP;
#define SET_WRITE(sorc, dest) req->type = DMA_WRITE_OP; src->mem_id = sorc; dst->mem_id = dest; dma_req.op = DMA_WRITE_OP;
#define SET_CAS(sorc, dest)   req->type = DMA_CAS_OP;   src->mem_id = sorc; dst->mem_id = dest; dma_req.op = DMA_CAS_OP;
#define SET_FAA(sorc, dest)   req->type = DMA_FAA_OP;   src->mem_id = sorc; dst->mem_id = dest; dma_req.op = DMA_FAA_OP;

// Since the size of a CAS must be known at compile time, the size field of the packet buffer can be used
// to store the CAS operands without changing the structure of the packet. This allows the CAS operation to complete
// without editing the size of the packet buffer like in a read.
// NEW/OLD MUST BE <= 32 BITS EACH
#define CAS_PACK(old,new) ((uint64_t)(new) | ((uint64_t)(old) << 32))

#define BUCKET_OFFSET(bucket_index) sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * bucket_index)

typedef struct __attribute__((packed)) req_pkt {
  struct ethhdr eth;
  struct iphdr ip;
  struct udphdr udp;
  uint16_t param_size;
  uint64_t timestamp;
  uint64_t nseq;
  uint8_t server_type;
  uint8_t type;
  uint64_t mr_offset; // key
  uint64_t data_size;
  uint8_t data[0]; // value
} req_pkt_t;

typedef struct __attribute__((packed)) kv_pair
{
  uint8_t cmd;
  KEY_TYPE key;
  VAL_TYPE val;
} kv_pair;

// This generates a reply by flipping src and dst
inline void send_reply(void *data) {
  struct ethhdr *eth = (struct ethhdr *)((char *)data);
  struct iphdr *ip = (struct iphdr *)((char *)data + sizeof(struct ethhdr));
  struct udphdr *udp = (struct udphdr *)((char *)data + sizeof(struct ethhdr) + sizeof(struct iphdr));

  unsigned char eth_src[ETH_ALEN];
  uint32_t ip_src = ip->saddr;
  uint16_t udp_src = udp->source;

  // Swap eth addresses
  bpf_memcpy(eth_src, eth->h_source, ETH_ALEN);
  bpf_memcpy(eth->h_source, eth->h_dest, ETH_ALEN);
  bpf_memcpy(eth->h_dest, eth_src, ETH_ALEN);

  // Swap IP addresses
  ip->saddr = ip->daddr;
  ip->daddr = ip_src;

  // Swap UDP ports
  udp->source = udp->dest;
  udp->dest = udp_src;
}

uint64_t prog(void *pkt)
{
  uint64_t pid;
  struct iphdr *ip = (struct iphdr *)((char *)pkt + sizeof(struct ethhdr));
  size_t pkt_len = sizeof(struct ethhdr) + ntohs(ip->tot_len);

  req_pkt_t *req = (req_pkt_t *)pkt;

  dma_req_t dma_req;
  dma_addr_t *src = &dma_req.src_addr;
  dma_addr_t *dst = &dma_req.dst_addr;
  src->app_id = 1;
  dst->app_id = 1;
  dma_req.size = req->data_size;
    
  kv_pair kv;
  kv = *(kv_pair *)req->data;
    
  if (kv.cmd == REQT_GET) // READ
  {
    uint64_t key = kv.key;
    uint64_t key_hash;
    uint64_t new_len = pkt_len;
    bpf_hash(&key, 8, HASH_SEED, &key_hash);
    uint32_t bucket_index = (uint32_t)(key_hash >> 16) & 15U;
    uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);

    SET_READ(1,0)
    src->offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index;
    dma_req.size = sizeof(struct mehcached_bucket);
    dma_req.op = DMA_READ_OP;

    // Try to get a even version number to get unlocked bucket
    uint32_t version_counter;
    bool version_flag = false;
    uint32_t prev = 0;
    struct mehcached_bucket *current_bucket;
    for (version_counter = 0; version_counter < 2; version_counter++)
    {
      if(version_counter > 0) {
        DMA_READ(pkt, new_len, &dma_req);
      }
      else {
        DMA_READ(pkt, pkt_len, &dma_req);
        pkt_len -= sizeof(struct kv_pair);
      }

      new_len = sizeof(struct mehcached_bucket) + pkt_len;
      current_bucket = (struct mehcached_bucket *)(req->data);

      prev = current_bucket->version;
      if (prev & 1U != 0U)
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
      goto end;
    }
    
    version_flag = false;
    // Serach item vector for the key
    VAL_TYPE user_val = NULL;
    for (int item_index = 0; item_index < MEHCACHED_ITEMS_PER_BUCKET; item_index++)
    {
      if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) != tag)
        continue;

      // Found, read the item
      //req->data_size = sizeof(struct mehcached_item);
      dma_req.size = sizeof(struct mehcached_item);
      src->offset = MEHCACHED_ITEM_OFFSET(current_bucket->item_vec[item_index]) + sizeof(uint64_t) + (sizeof(struct mehcached_bucket) * NUM_BUCKETS);
      DMA_READ(pkt, new_len, &dma_req);
      new_len = sizeof(struct mehcached_item) + pkt_len;

      /*struct mehcached_item *item = (struct mehcached_item *)(req->data -8);*/
      struct mehcached_item *item = (struct mehcached_item *)(req->data);

      if (item->key_hash != key_hash)
      {
        continue;
      }
      user_val = item->val;
      break;
    }

    if(user_val == NULL) {
      DPrintf("GET %lu: KEY NOT FOUND\n", kv.key);
      goto end;
    }

    DPrintf("GET %lu: KEY FOUND VAL -> %lu\n", kv.key, user_val);
    
    for (version_counter = 0; version_counter < 2; version_counter++)
    {

      src->offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index;
      //req->data_size = sizeof(struct mehcached_bucket);
      dma_req.size = sizeof(struct mehcached_bucket);

      DMA_READ(pkt, new_len, &dma_req);
      new_len = sizeof(struct mehcached_bucket) + pkt_len;

      current_bucket = (struct mehcached_bucket *)(req->data);
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
      DPrintf("GET %lu: VERSION FAILED\n", kv.key);
      goto end;
    }
  }

  DPrintf("GET %lu: SUCCESS\n", kv.key);

end:
  send_reply(pkt);
  return 0;
}
