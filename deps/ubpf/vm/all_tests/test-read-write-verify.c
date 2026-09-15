/*
 * Test: Verify dma_read and dma_write helper functions
 * Input packet: write.pkt / read.pkt
 * 
 */

#include "naam.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(char) > pkt_end)
    return 1;
  
  int offset = APP_REGION_OFFSET(pkt);

  // Write 'X' in the packet buffer application region
  // This will be copied to the memory region through DMA
  *(char *)APP_REGION_PTR(pkt) = 'X';

  // Write 1 byte of data to the memory region offset 16
  int ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, 16), DMA_ADDR(1, 0, offset), 1);

  if (ret == RET_DMA_FAILURE)
    return 1;
  
  // Clear the value 'X' in the packet buffer application region
  *(char *)APP_REGION_PTR(pkt) = 0;
  
  ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, 16), 1);
  
  if (ret == RET_DMA_FAILURE)
    return 1;
  
  if (*(char *)APP_REGION_PTR(pkt) != 'X')
    return 1;

  return 0;
}
