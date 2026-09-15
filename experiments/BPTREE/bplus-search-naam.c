/*
 * B+ Tree search
 */

#include "naam.h"

#define MAX_TREE_DEPTH  10
#define MAX_KEYS_PER_NODE 256
#define ROOT_OFFSET 18852085

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  dma_addr_t addr_from, addr_to;
  int i;
  
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(bpt_search_req_t) > pkt_end)
    return 1;

  uint64_t key = ((bpt_search_req_t *)APP_REGION_PTR(pkt))->key;
  int offset_free = APP_REGION_OFFSET(pkt) + sizeof(bpt_search_req_t);

  addr_from = DMA_ADDR(1, 1, ROOT_OFFSET); // Address of the root node
  addr_to = DMA_ADDR(1, 0, offset_free); // offset in packet buffer to write the fetched node

  // fetch root
  if (DMA_READ(pkt, addr_to, addr_from, sizeof(Node)) != RET_DMA_SUCCESS)
  {
    DPrintf("Error: Reading root node failed\n");
    goto end;
  }
    
  Node *root = (Node *)((char *)APP_REGION_PTR(pkt) + sizeof(bpt_search_req_t));

  // traverse the B+ tree until we reach a leaf node
  for (i = 0; i < MAX_TREE_DEPTH; i++) {
    if (root->leaf) {
      break;
    }
    
    // find the first key greater than or equal to the search key
    int j = 0;
    for (; j < MAX_KEYS_PER_NODE; j++){
      if (j >= root->n) {
        break;
      }
      if (key < root->keys[j]) {
        break;
      }
    }

    // fetch the child node
    addr_from = DMA_ADDR(1, 1, root->children[j]);
    addr_to = DMA_ADDR(1, 0, offset_free);
    
    if (DMA_READ(pkt, addr_to, addr_from, sizeof(Node)) != RET_DMA_SUCCESS)
    {
      DPrintf("Error: Reading child node failed\n", key);
      goto end;
    }
    
    root = (Node *)((char *)APP_REGION_PTR(pkt) + sizeof(bpt_search_req_t));
  }

  int key_found = 0;

  // linear search in the leaf node
  for (i = 0; i < MAX_KEYS_PER_NODE; i++) {
    if (i >= root->n) {
      break;
    }

    if (root->keys[i] == key) {
      key_found = 1;
      break;
    }
  }

  if (key_found) {
    DPrintf("Key found\n");
  } else {
    DPrintf("Key not found\n");
  }

end:
  send_reply(pkt);
  return 0;
}
