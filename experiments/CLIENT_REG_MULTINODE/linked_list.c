/*
 * Program: Linked list traversal
 */
#include "naam.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  uint8_t ret;
  
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(struct llnode) > pkt_end)
    return 1;

  // Number of nodes to walk. Fetched from the packet buffer
  int num_nodes = *(int *)APP_REGION_PTR(pkt);
  
  // Offset in the packet buffer where fetched nodes
  // will be stored
  int dst_offset = APP_REGION_OFFSET(pkt);
  
  // Offset in the memory region where the linked list starts
  int src_offset = 0;

  for (uint32_t i = 0; i < num_nodes; i++) {
    // Each linked list struct is 8 bytes
    // DMA_READ will copy 8 bytes from the memory region
    ret = DMA_READ(pkt, DMA_ADDR(1, 0, dst_offset), DMA_ADDR(1, 1, src_offset), 8);

    // Next node in the list
    struct llnode *node = (struct llnode *)APP_REGION_PTR(pkt);
    src_offset = node->offset;
  }

  send_reply(pkt);
  
  return 0;
}
