/*
 * Test: Pointer to packet buffer after yield
 * Input packet: write.pkt / read.pkt
 *
 * Pointer adress might change after helper function call
 * for r6. This info is from the verifier. This needs to passed
 * with the bit vector
 */

#include "naam.h"

// r6 contains a pointer to packet buffer fix it
#define BITVECT 0x0000000000000001

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
  DMA_READ_TRUSTED(pkt, BITVECT, 0, 0, 0);
  
  // Check if the value is still there
  // after the yield
  if (*key != 0xdeadbeef) {
    return 1;
  }
  
  return 0;
}
