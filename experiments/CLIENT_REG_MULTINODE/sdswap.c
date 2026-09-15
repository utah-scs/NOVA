/*
 * BPF code to swap src/dst and reply
 */

#include "naam.h"
#include "common.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  void *pkt = (void *)(long)ctx->data; 
  void *pkt_end = (void *)(long)ctx->data_end; 
  
  if (pkt + offsetof(req_pkt_t, data) + sizeof(get_set_req_t) > pkt_end)
    return 1;
  
  send_reply(pkt);
  
  return 0;
}
