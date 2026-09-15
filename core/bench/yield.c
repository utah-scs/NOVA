/*
 * Test program that just yields
 * Test if function return properly after yield
 */

#include "naam.h"

uint64_t prog(void *pkt) {
  // yield with fake DMA request
  bpf_yield(pkt, 0);

  return 0;
}
