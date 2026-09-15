/*
 * Test: Stack pointer after yield 
 * Input packet: any
 *
 */

#include "naam.h"

typedef struct __attribute__((packed)) app_data {
  uint8_t op_type;
  uint64_t mr_offset;
  uint64_t data_size;
} app_data_t;

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  volatile int x = 0xabcd;
  volatile int *y = &x;
  
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 

  if (pkt + offsetof(req_pkt_t, data) + sizeof(app_data_t) > pkt_end)
    return 1;
  
  // yield with fake DMA request
  DMA_READ(pkt, 0, 0, 0);

  if (*y != 0xabcd)
      return 1;

  return 0;
}
