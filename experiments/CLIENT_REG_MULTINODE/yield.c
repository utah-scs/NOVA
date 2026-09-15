/*
 * Benchmark cost of yield 
 */

#include "naam.h"
#include "common.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(get_set_req_t) > pkt_end)
    return 1;
  
  // yield with bogus DMA request
  DMA_READ(pkt, 0, 0, 0);

  send_reply(pkt);
  
  return 0;
}
