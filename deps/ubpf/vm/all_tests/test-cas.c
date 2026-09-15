/*
 * Test: Test CAS
 * Input packet: read.pkt / write.pkt
 * 
 * TODO: verifier failing
 *
 * 50:51: Invalid type (r1.type == number)
 * 72:73: Invalid type (r1.type == number)
 */

#include "naam.h"

typedef struct __attribute__((packed)) app_data {
  uint32_t value;
} app_data_t;

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 

  if (pkt + offsetof(req_pkt_t, data) + sizeof(app_data_t) > pkt_end)
    return 1;
  
  app_data_t *app_req = (app_data_t *)APP_REGION_PTR(pkt);
  app_req->value = 3;
  int offset = APP_REGION_OFFSET(pkt);

  // Write initial value in the memory region
  int ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, 0), DMA_ADDR(1, 0, offset), sizeof(app_req->value));

  if (ret != RET_DMA_SUCCESS)
    return 1;

  // CAS should be successful and return 3
  ret = DMA_CAS(pkt, DMA_ADDR(1, 1, 0), 3, 2);
  
  if (ret != 3)
    return 1;
  
  // CAS should fail and return 2
  ret = DMA_CAS(pkt, DMA_ADDR(1, 1, 0), 3, 2);

  if (ret != 2)
    return 1;

  // Final memory region value should be 2
  // Read value and check
  ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, 0), sizeof(app_req->value));

  if (ret != RET_DMA_SUCCESS)
    return 1;

  if (app_req->value != 2)
    return 1;

  return 0;
}
