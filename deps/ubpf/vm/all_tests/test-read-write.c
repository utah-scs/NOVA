/*
 * Test: dma_read and dma_write helper functions
 * Input packet: write.pkt/read.pkt
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
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(app_data_t) > pkt_end)
    return 1;
  
  app_data_t *app_req = (app_data_t *)APP_REGION_PTR(pkt);
  int offset = APP_REGION_OFFSET(pkt) + sizeof(app_data_t);
  int ret;
  
  if (app_req->op_type == REQT_SET) {
    ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, app_req->mr_offset), DMA_ADDR(1, 0, offset), app_req->data_size);
  } else {
    ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, app_req->mr_offset), app_req->data_size);
  }

  send_reply(pkt);
  
  if (ret != RET_DMA_SUCCESS)
    return 1;

  return 0;
}
