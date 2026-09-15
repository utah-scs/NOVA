#include <stdio.h>
#include <string.h>
#include "ubpf_ext.h"
#include "ubpf_int.h"
#include "naam.h"

void store_vm_state(struct ubpf_vm *vm, uint64_t *regs, uint64_t pc) {
  vm->state->pc = pc;

  /* Store registers */
  memcpy(vm->state->regs, regs, sizeof(uint64_t) * 16);
  /*printf("Stored vm state in packet\n");*/
  /*dump_vm_state_pkt(vm->state);*/
}

void restore_vm_state(struct ubpf_vm *vm, void *mem) {
    /* offset to vm states */
    int offset_state = offsetof(req_pkt_t, state);

    /* Restore vm state from packet */
    vm->state = (struct vm_state *)((char *)mem + offset_state);
    /*printf("Restored vm state from packet\n");*/
    /*dump_vm_state_pkt(vm->state);*/
}

int get_dma_status(void *mem) {
    int offset_dma_req = offsetof(req_pkt_t, dma_req);
    dma_req_t *dma_desc = (dma_req_t *)((char *)mem + offset_dma_req);

    return dma_desc->status;
}

bool preempted_helper(struct ubpf_vm *vm, int32_t imm) {
    if (strcmp(vm->ext_func_names[imm], "dma_read") == 0
            || strcmp(vm->ext_func_names[imm], "dma_write") == 0) {
    
        return true; 
    }

    return false;
}

#define MKOPNAME(name) [name] = #name,
const char* debug_opnames[] = {
    MKOPNAME(EBPF_OP_ADD_IMM)
    MKOPNAME(EBPF_OP_ADD_REG)
    MKOPNAME(EBPF_OP_SUB_IMM)
    MKOPNAME(EBPF_OP_SUB_REG)
    MKOPNAME(EBPF_OP_MUL_IMM)
    MKOPNAME(EBPF_OP_MUL_REG)
    MKOPNAME(EBPF_OP_DIV_IMM)
    MKOPNAME(EBPF_OP_DIV_REG)
    MKOPNAME(EBPF_OP_OR_IMM)
    MKOPNAME(EBPF_OP_OR_REG)
    MKOPNAME(EBPF_OP_AND_IMM)
    MKOPNAME(EBPF_OP_AND_REG)
    MKOPNAME(EBPF_OP_LSH_IMM)
    MKOPNAME(EBPF_OP_LSH_REG)
    MKOPNAME(EBPF_OP_RSH_IMM)
    MKOPNAME(EBPF_OP_RSH_REG)
    MKOPNAME(EBPF_OP_NEG)
    MKOPNAME(EBPF_OP_MOD_IMM)
    MKOPNAME(EBPF_OP_MOD_REG)
    MKOPNAME(EBPF_OP_XOR_IMM)
    MKOPNAME(EBPF_OP_XOR_REG)
    MKOPNAME(EBPF_OP_MOV_IMM)
    MKOPNAME(EBPF_OP_MOV_REG)
    MKOPNAME(EBPF_OP_ARSH_IMM)
    MKOPNAME(EBPF_OP_ARSH_REG)
    MKOPNAME(EBPF_OP_LE)
    MKOPNAME(EBPF_OP_BE)

    MKOPNAME(EBPF_OP_ADD64_IMM)
    MKOPNAME(EBPF_OP_ADD64_REG)
    MKOPNAME(EBPF_OP_SUB64_IMM)
    MKOPNAME(EBPF_OP_SUB64_REG)
    MKOPNAME(EBPF_OP_MUL64_IMM)
    MKOPNAME(EBPF_OP_MUL64_REG)
    MKOPNAME(EBPF_OP_DIV64_IMM)
    MKOPNAME(EBPF_OP_DIV64_REG)
    MKOPNAME(EBPF_OP_OR64_IMM)
    MKOPNAME(EBPF_OP_OR64_REG)
    MKOPNAME(EBPF_OP_AND64_IMM)
    MKOPNAME(EBPF_OP_AND64_REG)
    MKOPNAME(EBPF_OP_LSH64_IMM)
    MKOPNAME(EBPF_OP_LSH64_REG)
    MKOPNAME(EBPF_OP_RSH64_IMM)
    MKOPNAME(EBPF_OP_RSH64_REG)
    MKOPNAME(EBPF_OP_NEG64)
    MKOPNAME(EBPF_OP_MOD64_IMM)
    MKOPNAME(EBPF_OP_MOD64_REG)
    MKOPNAME(EBPF_OP_XOR64_IMM)
    MKOPNAME(EBPF_OP_XOR64_REG)
    MKOPNAME(EBPF_OP_MOV64_IMM)
    MKOPNAME(EBPF_OP_MOV64_REG)
    MKOPNAME(EBPF_OP_ARSH64_IMM)
    MKOPNAME(EBPF_OP_ARSH64_REG)

    MKOPNAME(EBPF_OP_LDXW)
    MKOPNAME(EBPF_OP_LDXH)
    MKOPNAME(EBPF_OP_LDXB)
    MKOPNAME(EBPF_OP_LDXDW)
    MKOPNAME(EBPF_OP_STW)
    MKOPNAME(EBPF_OP_STH)
    MKOPNAME(EBPF_OP_STB)
    MKOPNAME(EBPF_OP_STDW)
    MKOPNAME(EBPF_OP_STXW)
    MKOPNAME(EBPF_OP_STXH)
    MKOPNAME(EBPF_OP_STXB)
    MKOPNAME(EBPF_OP_STXDW)
    MKOPNAME(EBPF_OP_LDDW)

    MKOPNAME(EBPF_OP_JA)
    MKOPNAME(EBPF_OP_JEQ_IMM)
    MKOPNAME(EBPF_OP_JEQ_REG)
    MKOPNAME(EBPF_OP_JGT_IMM)
    MKOPNAME(EBPF_OP_JGT_REG)
    MKOPNAME(EBPF_OP_JGE_IMM)
    MKOPNAME(EBPF_OP_JGE_REG)
    MKOPNAME(EBPF_OP_JSET_REG)
    MKOPNAME(EBPF_OP_JSET_IMM)
    MKOPNAME(EBPF_OP_JNE_IMM)
    MKOPNAME(EBPF_OP_JNE_REG)
    MKOPNAME(EBPF_OP_JSGT_IMM)
    MKOPNAME(EBPF_OP_JSGT_REG)
    MKOPNAME(EBPF_OP_JSGE_IMM)
    MKOPNAME(EBPF_OP_JSGE_REG)
    MKOPNAME(EBPF_OP_CALL)
    MKOPNAME(EBPF_OP_EXIT)
    MKOPNAME(EBPF_OP_JLT_IMM)
    MKOPNAME(EBPF_OP_JLT_REG)
    MKOPNAME(EBPF_OP_JLE_IMM)
    MKOPNAME(EBPF_OP_JLE_REG)
    MKOPNAME(EBPF_OP_JSLT_IMM)
    MKOPNAME(EBPF_OP_JSLT_REG)
    MKOPNAME(EBPF_OP_JSLE_IMM)
    MKOPNAME(EBPF_OP_JSLE_REG)
};
#undef MKOPNAME

void debug_dump_inst(struct ubpf_vm* vm, int pc) {
    uint64_t *reg = vm->state->regs;
    struct ebpf_inst inst = vm->insts[pc];

    printf("== Next Instruction ==\n");
    printf("  pc 0x%x (%d) opcode %u (%s)\n",
         pc, pc, inst.opcode, debug_opnames[inst.opcode]);
    printf("  dst %u src %u offset 0x%x (%d) imm 0x%x (%d)\n",
         inst.dst, inst.src, inst.offset, inst.offset, inst.imm, inst.imm);
    printf("  srcval %lx dstval %lx\n", reg[inst.src], reg[inst.dst]);
}

void dump_vm_state(struct ubpf_vm *vm, int pc) {
    printf("== VM State ==\n");
    printf("  pc 0x%x (%d)\n", pc, pc); // Not in vm->m_state.pc except in ubpf_exec()
    printf("  sp 0x%lx (r10)\n", vm->state->regs[10]);

    for (int i = 0; i < 16; i++) {
        uint64_t r = vm->state->regs[i];
        printf("  r%d 0x%lx (%ld)\n", i, r, r);
    }
  
    printf("\n");
}

void dump_vm_state_pkt(struct vm_state *state) {
    printf("== VM State ==\n");
    printf("  pc 0x%lx (%lu)\n", state->pc, state->pc); // Not in vm->m_state.pc except in ubpf_exec()
    printf("  sp 0x%lx (r10)\n", state->regs[10]);

    for (int i = 0; i < 16; i++) {
        uint64_t r = state->regs[i];
        printf("  r%d 0x%lx (%ld)\n", i, r, r);
    }
  
    printf("\n");
}

int dma_req_size() {
  return DMA_REQ_SIZE;
}

void hex_dump(const char* desc, const void* addr,
    const int len, int per_line) {

    // Silently ignore silly per-line values.
    if (per_line < 4 || per_line > 64) per_line = 16;

    int i;
    unsigned char buff[per_line+1];
    const unsigned char * pc = (const unsigned char *)addr;

    // Output description if given.
    if (desc != NULL) printf ("%s:\n", desc);

    // Length checks.
    if (len == 0) {
        printf("  ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printf("  NEGATIVE LENGTH: %d\n", len);
        return;
    }

    // Process every byte in the data.
    for (i = 0; i < len; i++) {
        // Multiple of perLine means new or first line (with line offset).
        if ((i % per_line) == 0) {
            // Only print previous-line ASCII buffer for lines beyond first.
            if (i != 0) printf ("  %s\n", buff);
            
            // Output the offset of current line.
            printf ("  %04x ", i);
        }

        // Now the hex code for the specific character.
        printf (" %02x", pc[i]);

        // And buffer a printable ASCII character for later.
        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) // isprint() may be better.
            buff[i % per_line] = '.';
        else
            buff[i % per_line] = pc[i];
        buff[(i % per_line) + 1] = '\0';
    }

    // Pad out last line if not exactly perLine characters.
    while ((i % per_line) != 0) {
        printf ("   ");
        i++;
    }

    // And print the final ASCII buffer.
    printf ("  %s\n", buff);
}
