/*
 * Test program that just yields
 * Test if function return properly after yield
 */

#include "naam.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  if (data + offsetof(req_pkt_t, data) > data_end)
    return 1;
  
  // yield with fake DMA request
  DMA_READ(data, 0, 0, 0);

  return 0;
}
