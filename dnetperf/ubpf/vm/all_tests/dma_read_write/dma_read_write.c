/*
 * DMA read/write benchmark
 */

#include "naam.h"
#include "common.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(get_set_req_t) > pkt_end)
    return 1;
  
  get_set_req_t *app_req = (get_set_req_t *)APP_REGION_PTR(pkt);
  int offset = APP_REGION_OFFSET(pkt) + sizeof(get_set_req_t);
  int ret;
  
  if (app_req->op_type == REQT_SET) {
    ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, app_req->mr_offset), DMA_ADDR(1, 0, offset), app_req->data_size);
  } else {
    ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, app_req->mr_offset), app_req->data_size);
  }

  if (ret != RET_DMA_SUCCESS) {
    printk("Error: DMA read/wirte benchmark failed\n");
    return ret;
  }

  printk("Success! Another coming!\n");
  
  if (app_req->op_type == REQT_SET) {
    ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, app_req->mr_offset), DMA_ADDR(1, 0, offset), app_req->data_size);
  } else {
    ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, app_req->mr_offset), app_req->data_size);
  }

  if (ret != RET_DMA_SUCCESS) {
    printk("Error: DMA read/wirte benchmark failed\n");
    return ret;
  }
  
  printk("Success! End!\n");

  send_reply(pkt);
  
  return 0;
}
