/*
 * Copyright 2015 Big Switch Networks, Inc
 * Copyright 2017 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <elf.h>
#include <math.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdarg.h>
#include "ubpf.h"
#include "naam.h"
#include "ubpf_ext.h"

#define UNUSED __attribute__ ((unused))

void *memory_region;

void ubpf_set_register_offset(int x);
static void *readfile(const char *path, size_t maxlen, size_t *len);
static void register_functions(struct ubpf_vm *vm);

static void usage(const char *name)
{
    fprintf(stderr, "usage: %s [-h] [-j|--jit] [-m|--mem PATH] [-c|--reloc] BINARY\n", name);
    fprintf(stderr, "\nExecutes the eBPF code in BINARY and prints the result to stdout.\n");
    fprintf(stderr, "If --mem is given then the specified file will be read and a pointer\nto its data passed in r1.\n");
    fprintf(stderr, "If --jit is given then the JIT compiler will be used.\n");
    fprintf(stderr, "\nOther options:\n");
    fprintf(stderr, "  -r, --register-offset NUM: Change the mapping from eBPF to x86 registers\n");
    fprintf(stderr, "  -U, --unload: unload the code and reload it (for testing only)\n");
    fprintf(stderr, "  -R, --reload: reload the code, without unloading it first (for testing only, this should fail)\n");
    fprintf(stderr, "  -c, --reloc: Test packet buffer address change\n");
}

int main(int argc, char **argv)
{
    struct option longopts[] = {
        { .name = "help", .val = 'h', },
        { .name = "mem", .val = 'm', .has_arg=1 },
        { .name = "jit", .val = 'j' },
        { .name = "register-offset", .val = 'r', .has_arg=1 },
        { .name = "unload", .val = 'U' }, /* for unit test only */
        { .name = "reload", .val = 'R' }, /* for unit test only */
        { .name = "reloc", .val = 'c' }, /* for unit test only */
        { }
    };

    const char *mem_filename = NULL;
    bool jit = false;
    bool unload = false;
    bool reload = false;
    bool reloc = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "hm:jr:URc", longopts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            mem_filename = optarg;
            break;
        case 'j':
            jit = true;
            break;
        case 'r':
            ubpf_set_register_offset(atoi(optarg));
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        case 'U':
            unload = true;
            break;
        case 'R':
            reload = true;
            break;
        case 'c':
            reloc = true;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (unload && reload) {
        fprintf(stderr, "-U and -R can not be used together\n");
        return 1;
    }

    if (argc != optind + 1) {
        usage(argv[0]);
        return 1;
    }

    const char *code_filename = argv[optind];
    size_t code_len;
    void *code = readfile(code_filename, 1024*1024, &code_len);
    if (code == NULL) {
        return 1;
    }

    size_t mem_len = 0;
    void *mem = NULL;
    if (mem_filename != NULL) {
        mem = readfile(mem_filename, 1500, &mem_len);
        if (mem == NULL) {
            return 1;
        }
    }

    struct xdp_md ctx;
    ctx.data = (long)mem;

    // Setup memory region for simulating DMA engine
    int mem_reg_size = 1024 * 1024;
    memory_region = malloc(mem_reg_size);
    memset(memory_region, 0, mem_reg_size);

    // For dma_read test
    strcpy((char *)memory_region + 16, "helloworld");

    // For hashtable
    //*(uint64_t*)memory_region = sizeof(uint64_t) + NUM_BUCKETS * 128; //TODO move all relevant definitions to naam.h

    struct ubpf_vm *vm = ubpf_create();
    if (!vm) {
        fprintf(stderr, "Failed to create VM\n");
        return 1;
    }

    register_functions(vm);

    /* 
     * The ELF magic corresponds to an RSH instruction with an offset,
     * which is invalid.
     */
    bool elf = code_len >= SELFMAG && !memcmp(code, ELFMAG, SELFMAG);

    char *errmsg;
    int rv;
load:
    if (elf) {
      rv = ubpf_load_elf(vm, code, code_len, &errmsg);
    } else {
      rv = ubpf_load(vm, code, code_len, &errmsg);
    }
    if (unload) {
        ubpf_unload_code(vm);
        unload = false;
        goto load;
    }
    if (reload) {
        reload = false;
        goto load;
    }

    free(code);

    if (rv < 0) {
        fprintf(stderr, "Failed to load code: %s\n", errmsg);
        free(errmsg);
        ubpf_destroy(vm);
        return 1;
    }

    uint64_t ret;

    if (jit) {
        ubpf_jit_fn fn = ubpf_compile(vm, &errmsg);
        if (fn == NULL) {
            fprintf(stderr, "Failed to compile: %s\n", errmsg);
            free(errmsg);
            free(mem);
            return 1;
        }
        
        do {
            // dma_read might change packet length
            // Fetch updated packet length
            struct iphdr *ip = (struct iphdr *)((char *)mem + sizeof(struct ethhdr));
            mem_len = sizeof(struct ethhdr) + ntohs(ip->tot_len);
            ctx.data = (long)mem;
            ctx.data_end = (long)((char *)mem + mem_len);

            ret = fn(&ctx, mem_len); 
            
            printf("ret: 0x%"PRIx64"\n", ret);

            if (reloc && ret == RET_DMA_HELPER) {
                printf("Old packet address: %p\n", mem);
                void *new_mem = malloc(MAX_PKT_SZ);
                memcpy(new_mem, mem, MAX_PKT_SZ);
                free(mem);
                mem = new_mem;
                printf("New packet address: %p\n", mem);
            }
        }
        while (ret == RET_DMA_HELPER);
    
    } else {
        
        do {
            ubpf_exec(vm, mem, mem_len, &ret); 
            
            // dma_read might change packet length
            // Fetch updated packet length
            struct iphdr *ip = (struct iphdr *)((char *)mem + sizeof(struct ethhdr));
            mem_len = sizeof(struct ethhdr) + ntohs(ip->tot_len);
        }
        while (ret == RET_DMA_HELPER);
    }

    printf("Program returned with: 0x%"PRIx64"\n", ret);

    ubpf_destroy(vm);
    free(mem);

    return ret;
}

static void *readfile(const char *path, size_t maxlen, size_t *len)
{
    FILE *file;
    if (!strcmp(path, "-")) {
        file = fdopen(STDIN_FILENO, "r");
    } else {
        file = fopen(path, "r");
    }

    if (file == NULL) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    void *data = calloc(maxlen, 1);
    size_t offset = 0;
    size_t rv;
    while ((rv = fread(data+offset, 1, maxlen-offset, file)) > 0) {
        offset += rv;
    }

    if (ferror(file)) {
        fprintf(stderr, "Failed to read %s: %s\n", path, strerror(errno));
        fclose(file);
        free(data);
        return NULL;
    }

    if (!feof(file)) {
        fprintf(stderr, "Failed to read %s because it is too large (max %u bytes)\n",
                path, (unsigned)maxlen);
        fclose(file);
        free(data);
        return NULL;
    }

    fclose(file);
    if (len) {
        *len = offset;
    }
    return data;
}

#ifndef __GLIBC__
void *
memfrob(void *s, size_t n)
{
    for (int i = 0; i < n; i++) {
        ((char *)s)[i] ^= 42;
    }
    return s;
}
#endif

static uint64_t
gather_bytes(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e)
{
    return ((uint64_t)a << 32) |
        ((uint32_t)b << 24) |
        ((uint32_t)c << 16) |
        ((uint16_t)d << 8) |
        e;
}

static void
trash_registers(void)
{
    /* Overwrite all caller-save registers */
#if __x86_64__
    asm(
        "mov $0xf0, %rax;"
        "mov $0xf1, %rcx;"
        "mov $0xf2, %rdx;"
        "mov $0xf3, %rsi;"
        "mov $0xf4, %rdi;"
        "mov $0xf5, %r8;"
        "mov $0xf6, %r9;"
        "mov $0xf7, %r10;"
        "mov $0xf8, %r11;"
    );
#elif __aarch64__
    asm(
        "mov w0, #0xf0;"
        "mov w1, #0xf1;"
        "mov w2, #0xf2;"
        "mov w3, #0xf3;"
        "mov w4, #0xf4;"
        "mov w5, #0xf5;"
        "mov w6, #0xf6;"
        "mov w7, #0xf7;"
        "mov w8, #0xf8;"
        "mov w9, #0xf9;"
        "mov w10, #0xfa;"
        "mov w11, #0xfb;"
        "mov w12, #0xfc;"
        "mov w13, #0xfd;"
        "mov w14, #0xfe;"
        "mov w15, #0xff;"
        ::: "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7", "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15"
    );
#else
    fprintf(stderr, "trash_registers not implemented for this architecture.\n");
    exit(1);
#endif
}


static uint32_t
sqrti(uint32_t x)
{
    return sqrt(x);
}


static uint64_t
unwind(uint64_t i)
{
    return i;
}

void dump_dma_addr(dma_addr_t addr) {
  printf("<%u, %u, %lu>", DMA_ADDR_APP_ID(addr), DMA_ADDR_MEM_ID(addr), DMA_ADDR_OFFSET(addr));
}

void dump_dma_req(dma_req_t* req) {
  if (req->op == DMA_READ_OP || req->op == DMA_WRITE_OP) {
    printf("dma_req { op=%s, src=", req->op ? "WRITE" : "READ");
    dump_dma_addr(req->src_addr);
    printf(", dst=");
    dump_dma_addr(req->dst_addr);
    printf(", size=%lu }\n", req->size);
  }
  else if (req->op == DMA_CAS_OP) {
    printf("dma_req { op=%s, dst=", "CAS");
    dump_dma_addr(req->src_addr);
    printf(", oldval=%lu, newval=%lu }\n",
        CAS_UNPACK_OLDVAL(req->size), CAS_UNPACK_NEWVAL(req->size));
  }
  else if (req->op == DMA_FAA_OP) {
    printf("dma_req { op=%s, dst=", "FAA");
    dump_dma_addr(req->src_addr);
    printf(", val=%lu }\n", req->size);
  }
  else {
    printf("dma_req { op=%s }\n", "INVALID");
  }
}

void print_n_bytes(char *buf, size_t n) {
  for (int i = 0; i < n; i++) {
    printf("%02x ", buf[i]);
  }
  printf("\n");
}

uint64_t dma_read(uint64_t *pkt) {
  req_pkt_t *req_pkt = (req_pkt_t *)pkt;
  dma_req_t *req_dma = &req_pkt->dma_req;
  
  struct iphdr *ip = (struct iphdr *)((char *)pkt + sizeof(struct ethhdr));
  size_t pkt_len = sizeof(struct ethhdr) + ntohs(ip->tot_len);

  dump_dma_req(req_dma);
  
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
            printf("DMA_READ: pkt len increase old -> %lu, new -> %lu\n", pkt_len, new_len);
          }

          // Copy data from memory to packet buffer
          memcpy((char *)pkt + dst_off, (char *)memory_region + src_off, size);

          // Update status to success
          req_dma->status = RET_DMA_SUCCESS;

          printf("DMA_READ: memory region data -> ");
          print_n_bytes((char *)memory_region + src_off, size);
          printf("DMA_READ: Data in packet buf -> ");
          print_n_bytes((char *)pkt + dst_off, size);
        }
        else {
          // Update status to failure
          req_dma->status = RET_DMA_FAILURE;
          printf("DMA_READ: Invalid DMA region <Private filed || dma_region || MTU>\n");
        }
      }
      
      else {
        // Update status to success
        req_dma->status = RET_DMA_SUCCESS;
        printf("DMA_READ: No actual DMA read performed\n");
      }
    }
    
    else {
      // Update status to failure
      req_dma->status = RET_DMA_FAILURE;
      printf("DMA_READ: dst_addr.mem_id should be 0. Got %d\n", DMA_ADDR_MEM_ID(req_dma->dst_addr));
    }
  }
  
  else if (req_dma->op == DMA_CAS_OP) {
    CAS_TYPE ret = __sync_val_compare_and_swap(
                          (CAS_TYPE *)((char *)memory_region + dst_off),
                          (CAS_TYPE)CAS_UNPACK_OLDVAL(size),
                          (CAS_TYPE)CAS_UNPACK_NEWVAL(size));

    // Encode CAS return value to lower 32 bits of size field
    // of dma descriptor in packet buffer
    req_dma->size = CAS_PACK(0, ret);
  }

  else if (req_dma->op == DMA_FAA_OP) {
    uint64_t ret = __atomic_fetch_add(
                          (uint64_t *)((char *)memory_region + dst_off),
                          size, __ATOMIC_SEQ_CST);
                          

    // Push FAA return value to the size field
    // of dma descriptor in packet buffer
    req_dma->size = ret;
  }
  
  else {
    // Update status to failure
    req_dma->status = RET_DMA_FAILURE;
    printf("DMA_READ: Request OP should be zero for dma_read. Found %d\n", req_dma->op);
  }
  
  return RET_DMA_HELPER;
}

uint64_t dma_write(uint64_t *pkt) {
  req_pkt_t *req_pkt = (req_pkt_t *)pkt;
  dma_req_t *dma_write_req = &req_pkt->dma_req;
  
  dump_dma_req(dma_write_req);

  printf("packet data: %s\n", (char *)pkt + DMA_ADDR_OFFSET(dma_write_req->src_addr));
  memcpy((char *)memory_region + DMA_ADDR_OFFSET(dma_write_req->dst_addr), (char *)pkt + DMA_ADDR_OFFSET(dma_write_req->src_addr), dma_write_req->size);
  
  return RET_DMA_HELPER;
}

uint64_t dma_cas(UNUSED uint64_t *pkt, UNUSED uint64_t *pkt_len, uint64_t *dma_req) {
  dma_req_t *dma_cas_req = (dma_req_t *)dma_req;
  
  dump_dma_req(dma_cas_req);
  
  return RET_DMA_HELPER;
}

uint64_t bpf_memcpy(uint64_t *dest, uint64_t *src, uint64_t size) {
  (void)dest;
  (void)src;
  (void)size;
  return 0;
}

void bpf_hash(const uint64_t *key, const int size, uint64_t *hash, const int h_size, uint64_t seed) {
  (void)key;
  (void)size;
  (void)hash;
  (void)h_size;
  (void)seed;
}

long bpf_trace_printk(const char* fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  return vfprintf(stdout, fmt, argp);
}

static void
register_functions(struct ubpf_vm *vm)
{
    ubpf_register(vm, 6, "bpf_trace_printk", bpf_trace_printk);
    ubpf_register(vm, 186, "dma_read", dma_read);
    ubpf_register(vm, 187, "dma_write", dma_write);
    ubpf_register(vm, 188, "bpf_hash", bpf_hash);
    ubpf_register(vm, 189, "bpf_memcpy", bpf_memcpy);
    ubpf_register(vm, 0, "gather_bytes", gather_bytes);
    ubpf_register(vm, 1, "memfrob", memfrob);
    ubpf_register(vm, 2, "sqrti", sqrti);
    ubpf_register(vm, 5, "unwind", unwind);
    ubpf_register(vm, 8, "strcmp_ext", strcmp);
    ubpf_register(vm, 10, "trash_registers", trash_registers);
    ubpf_set_unwind_function_index(vm, 5);
}
