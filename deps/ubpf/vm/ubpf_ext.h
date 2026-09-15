#ifndef UBPF_EXT_H
#define UBPF_EXT_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

struct ubpf_vm;
struct vm_state;

/* 
 * Store VM state at the packet
 *
 * When preemptive helper functions are called store the VM state
 * at the end of the packet data.
 */
void store_vm_state(struct ubpf_vm *vm, uint64_t *regs, uint64_t pc);

/*
 * Restore VM state
 *
 * Restore the VM state from the packet
 */
void restore_vm_state(struct ubpf_vm *vm, void *mem);

/*
 * Get the DMA request status from the packet
 */
int get_dma_status(void *mem);

int dma_req_size();

/*
 * Return true if it's a preepmted function
 */
bool preempted_helper(struct ubpf_vm *vm, int32_t imm);

void debug_dump_inst(struct ubpf_vm *vm, int pc);

void dump_vm_state(struct ubpf_vm *vm, int pc);

void dump_vm_state_pkt(struct vm_state *state);

void hex_dump(const char* desc, const void* addr, const int len, int per_line);

#endif
