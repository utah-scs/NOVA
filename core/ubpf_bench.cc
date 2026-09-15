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

void DoSetup(const char* file) {
    char* addr = NULL;
    uint64_t code_length = bess::utils::load_bpf_code(file, &addr);
    
    vm = bess::utils::init_engine(addr, code_length);
}

void DoTeardown(){
    bess::utils::destroy_engine(vm);
}

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

void UbpfWriteTestNoJIT(benchmark::State& st){
    int ret;
    uint64_t bpf_ret_val;
    
    DoSetup("bench/dma_read_write.o");
    GenWritePayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, NULL, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }

    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfWriteTestJIT(benchmark::State& st){
    uint64_t ret = 0;
    uint64_t bpf_ret_val;
    ubpf_jit_fn jit_fn = NULL;
    
    DoSetup("bench/dma_read_write.o");
    GenWritePayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);

    //char *errmsg;
    //jit_fn = ubpf_compile(vm, &errmsg);
    //if (jit_fn == NULL) {
      //fprintf(stderr, "Failed to compile: %s\n", errmsg);
      //free(errmsg);
    //}
    
    jit_fn = bess::utils::jit_compile(vm);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, jit_fn, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }


    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfReadTestNoJIT(benchmark::State& st) {
    int ret;
    uint64_t bpf_ret_val;
    
    DoSetup("bench/dma_read_write.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, NULL, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }

    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfReadTestJIT(benchmark::State& st) {
    uint64_t ret = 0;
    uint64_t bpf_ret_val;
    ubpf_jit_fn jit_fn = NULL;
    
    DoSetup("bench/dma_read_write.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);

    //char *errmsg;
    //jit_fn = ubpf_compile(vm, &errmsg);
    //if (jit_fn == NULL) {
      //fprintf(stderr, "Failed to compile: %s\n", errmsg);
      //free(errmsg);
    //}
    jit_fn = bess::utils::jit_compile(vm);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, jit_fn, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }


    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfNopTestNoJIT(benchmark::State& st) {
    int ret;
    uint64_t bpf_ret_val = 0;
    
    DoSetup("bench/noop.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, NULL, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }

    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfNopTestJIT(benchmark::State& st) {
    uint64_t ret = 0;
    uint64_t bpf_ret_val = 1;
    ubpf_jit_fn jit_fn = NULL;
    
    DoSetup("bench/noop.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);

    //char *errmsg;
    //jit_fn = ubpf_compile(vm, &errmsg);
    //if (jit_fn == NULL) {
      //fprintf(stderr, "Failed to compile: %s\n", errmsg);
      //free(errmsg);
    //}
    jit_fn = bess::utils::jit_compile(vm);
    if (jit_fn == NULL) {
        printf("Failed to JIT compile\n");
        exit(EXIT_FAILURE);
    }
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, jit_fn, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }


    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfYieldTestNoJIT(benchmark::State& st) {
    int ret;
    uint64_t bpf_ret_val;
    
    DoSetup("bench/yield.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, NULL, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }

    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

void UbpfYieldTestJIT(benchmark::State& st) {
    uint64_t ret = 0;
    uint64_t bpf_ret_val;
    ubpf_jit_fn jit_fn = NULL;
    
    DoSetup("bench/yield.o");
    GenReadPayload();
    
    req_pkt_t *req_pkt = (req_pkt_t *)payload;
    struct xdp_md ctx;
    ctx.data = (long)payload;
    ctx.data_end = (long)(payload + payload_len);

    //char *errmsg;
    //jit_fn = ubpf_compile(vm, &errmsg);
    //if (jit_fn == NULL) {
      //fprintf(stderr, "Failed to compile: %s\n", errmsg);
      //free(errmsg);
    //}
    jit_fn = bess::utils::jit_compile(vm);
    if (jit_fn == NULL) {
        printf("Failed to JIT compile\n");
        exit(EXIT_FAILURE);
    }
    
    for(auto _ : st){
        memset(&req_pkt->state, 0, sizeof(struct vm_state));
        
        do {
          ret = bess::utils::run_bpf_code(vm, jit_fn, &ctx, payload_len, &bpf_ret_val);
          
          if(ret != 0){
              exit(EXIT_FAILURE);
          }
        } while(bpf_ret_val == RET_DMA_HELPER);
    }


    st.SetItemsProcessed(st.iterations());
    st.SetBytesProcessed(DMA_RW_SIZE * st.iterations());
    DoTeardown();
}

BENCHMARK(UbpfNopTestNoJIT)->Iterations(ITER);
BENCHMARK(UbpfNopTestJIT)->Iterations(ITER);
BENCHMARK(UbpfYieldTestNoJIT)->Iterations(ITER);
BENCHMARK(UbpfYieldTestJIT)->Iterations(ITER);
BENCHMARK(UbpfReadTestNoJIT)->Iterations(ITER);
BENCHMARK(UbpfReadTestJIT)->Iterations(ITER);
BENCHMARK(UbpfWriteTestNoJIT)->Iterations(ITER);
BENCHMARK(UbpfWriteTestJIT)->Iterations(ITER);

BENCHMARK_MAIN();
