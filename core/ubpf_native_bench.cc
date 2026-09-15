#include <benchmark/benchmark.h>
#include <glog/logging.h>
#include <iostream>
#include <vector>
#include <arpa/inet.h>
#include <functional>
#include "utils/ubpf_util.h"
#include "naam.h"
#include "../deps/ubpf/vm/inc/ubpf.h"
#include "ubpf_bench.h"

using namespace bess;

#define BUFFER 1500
#define JIT true
#define NOJIT false
#define ITER (1000 * 1000)

char payload[BUFFER];
int payload_len = BUFFER;
ubpf_vm* vm = NULL;

void *memory_region = malloc(1024 * 1024);

void GenReadPayload() {
	req_pkt_t *req_pkt = (req_pkt_t *)payload;
	get_set_entry_t *entry = (get_set_entry_t *)(req_pkt->data);
	
  req_pkt->nseq = 0;
	req_pkt->server_type = 0;
	
	entry->op_type = 0;
	entry->mr_offset = 16;
	entry->data_size = DMA_RW_SIZE;

  payload_len = sizeof(req_pkt_t) + sizeof(get_set_entry_t);
}

void GenWritePayload() {
	req_pkt_t *req_pkt = (req_pkt_t *)payload;
	get_set_entry_t *entry = (get_set_entry_t *)req_pkt->data;

	req_pkt->nseq = 0;
	req_pkt->server_type = 0;
	
	entry->op_type = 1;
	entry->mr_offset = 16;
	entry->data_size = DMA_RW_SIZE;
	
	// Copy data to write with dma write op into packet buffer
  char dma_data[DMA_RW_SIZE] = "Hello World!!!!";
	strncpy((char *)req_pkt->data + sizeof(get_set_entry_t), dma_data, DMA_RW_SIZE);

  payload_len = sizeof(req_pkt_t) + sizeof(get_set_entry_t) + DMA_RW_SIZE;
}

uint64_t __attribute__ ((noinline)) dma_read(uint64_t *pkt) {
  req_pkt_t *req_pkt = (req_pkt_t *)pkt;
  dma_req_t *req_dma = &req_pkt->dma_req;
  
  struct iphdr *ip = (struct iphdr *)((char *)pkt + sizeof(struct ethhdr));
  size_t pkt_len = sizeof(struct ethhdr) + ntohs(ip->tot_len);

  uint64_t src_off = DMA_ADDR_OFFSET(req_dma->src_addr);
  uint64_t dst_off = DMA_ADDR_OFFSET(req_dma->dst_addr);
  uint64_t size = req_dma->size;
  
  if (req_dma->op == DMA_READ_OP) {
    /*
     * If destination is packet buffer then copy data from memory region 
     * Otherwise invalid request
     */
    if (DMA_ADDR_MEM_ID(req_dma->dst_addr) == 0) {
      // Size 0 is used to test without real DMA
      if (size > 0) {
        
        if (dst_off >= offsetof(req_pkt_t, data) && (dst_off + size) < 1500) {
          
          // Needs to increase packet length first
          if (dst_off + size > pkt_len) {
            size_t new_len = dst_off + size;
            ip->tot_len = htons(new_len - sizeof(struct ethhdr));
          }

          // Copy data from memory to packet buffer
          memcpy((char *)pkt + dst_off, (char *)memory_region + src_off, size);

          // Update status to success
          req_dma->status = RET_DMA_SUCCESS;
          return RET_DMA_SUCCESS;
        }
        else {
          // Update status to failure
          req_dma->status = RET_DMA_FAILURE;
          printf("DMA_READ: Invalid DMA region <Private filed || dma_region || MTU>\n");
          return RET_DMA_FAILURE;
        }
      }
      else {
        // Update status to success
        req_dma->status = RET_DMA_SUCCESS;
        return RET_DMA_SUCCESS;
      }
    }
    else {
      // Update status to failure
      req_dma->status = RET_DMA_FAILURE;
      printf("DMA_READ: dst_addr.mem_id should be 0. Got %d\n", DMA_ADDR_MEM_ID(req_dma->dst_addr));
      return RET_DMA_FAILURE;
    }
  }
  else {
    // Update status to failure
    req_dma->status = RET_DMA_FAILURE;
    printf("DMA_READ: Request OP should be zero for dma_read. Found %d\n", req_dma->op);
    return RET_DMA_FAILURE;
  }
}

uint64_t __attribute__ ((noinline)) dma_write(uint64_t *pkt) {
  req_pkt_t *req_pkt = (req_pkt_t *)pkt;
  dma_req_t *dma_write_req = &req_pkt->dma_req;
  
  memcpy((char *)memory_region + DMA_ADDR_OFFSET(dma_write_req->dst_addr), (char *)pkt + DMA_ADDR_OFFSET(dma_write_req->src_addr), dma_write_req->size);

  return RET_DMA_SUCCESS;
}

uint64_t __attribute__ ((noinline)) FuncNAAMReadWrite(void *pkt) {
  get_set_entry_t *app_req = (get_set_entry_t *)APP_REGION_PTR(pkt);
  int offset = APP_REGION_OFFSET(pkt) + sizeof(get_set_entry_t);
  int ret;
  
  if (app_req->op_type == REQT_SET) {
    ret = DMA_WRITE((uint64_t *)pkt, DMA_ADDR(1, 1, app_req->mr_offset), DMA_ADDR(1, 0, offset), app_req->data_size);
  } else {
    ret = DMA_READ((uint64_t *)pkt, DMA_ADDR(1, 0, offset), DMA_ADDR(1, 1, app_req->mr_offset), app_req->data_size);
  }

  if (ret == RET_DMA_FAILURE)
    return 1;

  return 0;
}

uint64_t __attribute__ ((noinline)) FuncEmpty(void *pkt) {
  (void)pkt;
  return 0;
}

void UbpfNopTestNative(benchmark::State &st) {
    GenReadPayload();
    
    for (auto _ : st) {
      FuncEmpty((void *)payload);
    }
    
    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(0 * st.iterations());
}

void UbpfWriteTestNative(benchmark::State &st) {
    GenWritePayload();
    
    for (auto _ : st) {
      FuncNAAMReadWrite((void *)payload);
    }
    
    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
}

void UbpfReadTestNative(benchmark::State &st) {
    GenReadPayload();
    
    for (auto _ : st) {
      FuncNAAMReadWrite((void *)payload);
    }
    
    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
}

BENCHMARK(UbpfNopTestNative)->Iterations(ITER);
BENCHMARK(UbpfWriteTestNative)->Iterations(ITER);
BENCHMARK(UbpfReadTestNative)->Iterations(ITER);

BENCHMARK_MAIN();
