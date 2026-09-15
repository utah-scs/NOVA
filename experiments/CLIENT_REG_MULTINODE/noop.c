/*
 * NOOP bpf program
 */
#include "naam.h"

SECTION("xdp")
uint64_t prog(struct xdp_md *ctx) {
  return 0;
}
