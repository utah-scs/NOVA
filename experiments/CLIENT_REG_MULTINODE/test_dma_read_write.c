/*
 * BPF program for testing 
 * dma read/write operation
 * is working as exptected.
 */

#include "naam.h"
#include "common.h"

uint64_t prog(void *pkt) {
  int offset = APP_REGION_OFFSET(pkt);

  // Write 'X' in the packet buffer application region
  // This will be copied to the memory region through DMA
  *(char *)APP_REGION_PTR(pkt) = 'X';

  // Write 1 byte of data to the memory region offset 16
  int ret = DMA_WRITE(pkt, DMA_ADDR(1, 1, 16), DMA_ADDR(1, 0, offset), 1);

  if (ret == RET_DMA_FAILURE)
    printm("Error: DMA write failed\n");
  
  // Clear the value 'X' in the packet buffer application region
  *(char *)APP_REGION_PTR(pkt) = 0;
  
  ret = DMA_READ(pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, 16), 1);
  
  if (ret == RET_DMA_FAILURE)
    printm("Error: DMA read failed\n");
  
  if (*(char *)APP_REGION_PTR(pkt) != 'X')
    printm("Error: DMA read/write verification failed\n");

  send_reply(pkt);
  
  return 0;
}
