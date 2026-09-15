/*
 * Test: Print
 * Input packet: any
 *
 * TODO: Not supported by the verifier
 */

#include "naam.h"

uint64_t prog(void *pkt) {
  int x = 10;
  printk("%d\n", x);

  return 0;
}
