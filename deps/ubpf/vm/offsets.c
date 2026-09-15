#include <stdio.h>
#include "naam.h"

int main() {
  uint64_t base_addr = 0x555555564480;
  uint64_t state_addr = 0x555555564480 + offsetof(req_pkt_t, state);
  uint64_t stack_end_addr = state_addr + offsetof(struct vm_state, pc);
  uint64_t dma_state_addr = base_addr + offsetof(req_pkt_t, dma_req);

  size_t vm_state = offsetof(req_pkt_t, state);
  size_t vm_state_stack = offsetof(req_pkt_t, state) + offsetof(struct vm_state, stack);
  size_t vm_state_pc = offsetof(req_pkt_t, state) + offsetof(struct vm_state, pc);
  
  printf("off vm.state: 0x%lx (%lu)\n", vm_state, vm_state);
  printf("off vm.state.regs: 0x%lx (%lu)\n", vm_state, vm_state);
  printf("off vm.state.stack: 0x%lx (%lu)\n", vm_state_stack, vm_state_stack);
  printf("off vm.state.pc: 0x%lx (%lu)\n\n", vm_state_pc, vm_state_pc);

  size_t dma_state = offsetof(req_pkt_t, dma_req);
  size_t dma_state_src = offsetof(req_pkt_t, dma_req) + offsetof(dma_req_t, src_addr);
  size_t dma_state_dst = offsetof(req_pkt_t, dma_req) + offsetof(dma_req_t, dst_addr);
  size_t dma_state_size = offsetof(req_pkt_t, dma_req) + offsetof(dma_req_t, size);
  size_t dma_state_op = offsetof(req_pkt_t, dma_req) + offsetof(dma_req_t, op);
  size_t dma_state_status = offsetof(req_pkt_t, dma_req) + offsetof(dma_req_t, status);

  printf("off dma.state: 0x%lx (%lu)\n", dma_state, dma_state);
  printf("off dma.state.src_addr: 0x%lx (%lu)\n", dma_state_src, dma_state_src); 
  printf("off dma.state.dst_addr: 0x%lx (%lu)\n", dma_state_dst, dma_state_dst);
  printf("off dma.state.size: 0x%lx (%lu)\n", dma_state_size, dma_state_size);
  printf("off dma.state.op: 0x%lx (%lu)\n", dma_state_op, dma_state_op);
  printf("off dma.state.status: 0x%lx (%lu)\n\n", dma_state_status, dma_state_status);

  size_t param_size = offsetof(req_pkt_t, param_size);
  size_t timestamp = offsetof(req_pkt_t, timestamp);
  size_t nseq = offsetof(req_pkt_t, nseq);
  size_t server_type = offsetof(req_pkt_t, server_type);
  size_t data = offsetof(req_pkt_t, data);

  printf("off param_size: 0x%lx (%lu)\n", param_size, param_size);
  printf("off timestamp: 0x%lx (%lu)\n", timestamp, timestamp);
  printf("off nseq: 0x%lx (%lu)\n", nseq, nseq);
  printf("off server_type: 0x%lx (%lu)\n", server_type, server_type);
  printf("off data: 0x%lx (%lu)\n\n", data, data);

  printf("base address     : 0x%lx\n", base_addr);
  printf("vm state address : 0x%lx\n", state_addr);
  printf("stack end address: 0x%lx\n", stack_end_addr);
  printf("dma state address: 0x%lx\n", dma_state_addr);
}
