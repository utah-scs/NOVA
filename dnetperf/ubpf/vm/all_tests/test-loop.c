/*
 * Test: noop BPF program
 * Input packet: any
 */

#include <naam.h>

__attribute__((section("xdp"), used))
int prog(struct xdp_md *ctx) {
  void *data = (void *)(long)ctx->data;
  void *data_end = (void *)(long)ctx->data_end;

  if (data + MAX_PKT_SZ > data_end)
    return 1;

  for (int i = 0; i < 16; i++) {
    DMA_READ(data, 0, 0, 0);
  }

  return 0;
}
