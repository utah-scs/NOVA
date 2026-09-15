/*
 * Test: noop BPF program
 * Input packet: any
 */

#include <naam.h>

__attribute__((section("xdp"), used))
int prog(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  if (data + offsetof(req_pkt_t, data) > data_end)
    return 1;

  return 0;
}
