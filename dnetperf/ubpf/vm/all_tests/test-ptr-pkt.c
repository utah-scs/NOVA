/*
 * Test: Pointer to packet buffer after yield
 * Input packet: write.pkt / read.pkt
 */

#include "naam.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 

  if (pkt + offsetof(req_pkt_t, data) + sizeof(int) > pkt_end)
    return 1;
  
  // Insert some value in tha packet buffer
  // application region to check later
  *(int *)APP_REGION_PTR(pkt) = 0xdeadbeef;
  
  // Take a pointer to the application region
  int *key = (int *)APP_REGION_PTR(pkt);

  // yield with fake DMA request
  DMA_READ(pkt, 0, 0, 0);
  
  // Check if the value is still there
  // after the yield
  if (*key != 0xdeadbeef) {
    return 1;
  }
  
  return 0;
}
