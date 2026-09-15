#include <elf.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include <glog/logging.h>

#include "ubpf_util.h"
#include "../../deps/util/murmur.c"
extern "C" {
  #include <naam.h>
}
#define MEHCACHED_ROUNDUP8(x) (((x) + 7UL) & (~7UL))
namespace bess {
namespace utils {

struct rand_state {
  uint64_t a;
};

struct rand_state *r_state;
void *memory_region = malloc(1024 * 1024);

/* 
 * Insert DMA arguments at the packet tail 
 * to prepare for the DMA engine.
 * 
 * pkt => | headers | Data | VM State | DMA Params |
 */
void insert_DMA_params(void *pkt, size_t pkt_len, uint64_t *dma_req) {
  uint64_t offset = pkt_len - dma_req_size();

  dma_req_t *dma_req_param = (dma_req_t *)dma_req;
  
  memcpy((char *)pkt + offset, dma_req_param, sizeof(dma_req_t));
}

void dma_read_native(uint64_t *pkt) {
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
}

void dma_write_native(uint64_t *pkt) {
  req_pkt_t *req_pkt = (req_pkt_t *)pkt;
  dma_req_t *dma_write_req = &req_pkt->dma_req;
  
  memcpy((char *)memory_region + DMA_ADDR_OFFSET(dma_write_req->dst_addr), (char *)pkt + DMA_ADDR_OFFSET(dma_write_req->src_addr), dma_write_req->size);
}

uint64_t dma_read(uint64_t *pkt) {
  (void)pkt;  // unused

  // Uncomment for native execution
  // for ubpf_native_bench
  //dma_read_native(pkt);
  
  return RET_DMA_HELPER;
}

uint64_t  dma_write(uint64_t *pkt) {
  (void)pkt;  // unused 
  
  // Uncomment for native execution
  // for ubpf_native_bench
  //dma_write_native(pkt);
  
  return RET_DMA_HELPER;
}

uint64_t bpf_yield(uint64_t *pkt, uint64_t pkt_len) {
  (void)pkt;      // unused
  (void)pkt_len;  // unused
  return RET_DMA_HELPER;
}

uint64_t bpf_memcpy(void *dest, void *src, uint64_t size) {
  return (uint64_t)memcpy(dest,src, size);
}

uint64_t dump_hex(uint64_t* item, int len){
  for (int i = 0; i < len; i+=8){
    if(i > len){
      break;
    }
    printf("%08lx%08lx%08lx%08lx%08lx%08lx%08lx%08lx\n", item[i],item[i+1],item[i+2],item[i+3],item[i+7],item[i+5],item[i+6],item[i+7]);
  }
  return 0;
}

uint64_t bpf_memcmp(uint8_t *dest, uint8_t *src, uint64_t length) {
  length = MEHCACHED_ROUNDUP8(length);
  //printf("dest = %p src = %p\n", dest, src);
    switch (length >> 3)
    {
        case 0:
            return true;
        case 1:
            if (*(const uint64_t *)(dest + 0) != *(const uint64_t *)(src + 0))
                return false;
            return true;
        case 2:
            if (*(const uint64_t *)(dest + 0) != *(const uint64_t *)(src + 0))
                return false;
            if (*(const uint64_t *)(dest + 8) != *(const uint64_t *)(src + 8))
                return false;
            return true;
        case 3:
            if (*(const uint64_t *)(dest + 0) != *(const uint64_t *)(src + 0))
                return false;
            if (*(const uint64_t *)(dest + 8) != *(const uint64_t *)(src + 8))
                return false;
            if (*(const uint64_t *)(dest + 16) != *(const uint64_t *)(src + 16))
                return false;
            return true;
        case 4:
            if (*(const uint64_t *)(dest + 0) != *(const uint64_t *)(src + 0))
                return false;
            if (*(const uint64_t *)(dest + 8) != *(const uint64_t *)(src + 8))
                return false;
            if (*(const uint64_t *)(dest + 16) != *(const uint64_t *)(src + 16))
                return false;
            if (*(const uint64_t *)(dest + 24) != *(const uint64_t *)(src + 24))
                return false;
            return true;
        default:
            return memcmp(dest, src, length) == 0;
    }
}

// Random number generator to help debug
uint64_t bpf_rand() {
  uint64_t x = r_state->a;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return r_state->a = x;
}


uint64_t bpf_trace_printk(const char* fmt, uint32_t fmt_size, ...) {
  va_list argp;
  va_start(argp, fmt_size);
  vfprintf(stdout, fmt, argp);
  return 0;
}

// For the verifier helper functions need to provide size for each pointer
void bpf_hash(void *key, int key_len, void *key_hash, int hash_len, unsigned int seed) {
  (void)key_len;
  MurmurHash3_x86_128(key, hash_len, seed, key_hash);
}


static void register_functions(struct ubpf_vm *vm) {
  ubpf_register(vm, 6, "bpf_trace_printk", reinterpret_cast<void *>(bpf_trace_printk));
  ubpf_register(vm, 186, "dma_read", reinterpret_cast<void *>(dma_read));
  ubpf_register(vm, 187, "dma_write", reinterpret_cast<void *>(dma_write));
  ubpf_register(vm, 188, "bpf_hash", reinterpret_cast<void *>(bpf_hash));
  ubpf_register(vm, 189, "bpf_memcpy", reinterpret_cast<void *>(bpf_memcpy));
  ubpf_register(vm, 7, "bpf_yield", reinterpret_cast<void *>(bpf_yield));
  ubpf_register(vm, 8, "bpf_memcmp", reinterpret_cast<void *>(bpf_memcmp));
  ubpf_register(vm, 10, "dump_hex", reinterpret_cast<void *>(dump_hex));
  ubpf_register(vm, 11, "bpf_rand", reinterpret_cast<void *>(bpf_rand));

  return;
}

static void *readfile(const char *path, size_t maxlen, size_t *len) {
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
  while ((rv = fread((char *)data + offset, 1, maxlen - offset, file)) >
         0) {
    offset += rv;
  }

  if (ferror(file)) {
    fprintf(stderr, "Failed to read %s: %s\n", path, strerror(errno));
    fclose(file);
    free(data);
    return NULL;
  }

  if (!feof(file)) {
    fprintf(stderr,
            "Failed to read %s because it is too large (max %u bytes)\n", path,
            (unsigned)maxlen);
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

size_t load_bpf_code(const char *file_path, char **code_address) {
  size_t code_len = 0;
  
  *code_address = (char *)readfile(file_path, 5 * 1024 * 1024, &code_len);
  if (*code_address == NULL) {
    fprintf(stderr, "Couldn't load the file %s\n", file_path);
    return -1;
  }
  
  return code_len;
}

int destroy_engine(struct ubpf_vm *vm) {
  if (vm)
    free(vm);

  return 0;
}

/*
 * Creates a new VM 
 *
 * Returns a valid program ID on success, -1 on error
 */

struct ubpf_vm *init_engine(const char *code, size_t code_len) {
  struct ubpf_vm *vm = ubpf_create();
  
  // Enable packet memory and stack bounds check
  ubpf_toggle_bounds_check(vm, true);

  if (!vm) {
    fprintf(stderr, "Failed to create VM\n");
    return NULL;
  }
  
  // Initialize the random number generator for the
  // random number generator helper function
  r_state = (struct rand_state *)malloc(sizeof(struct rand_state));
  r_state->a = (uint64_t)time(NULL);

  register_functions(vm);

  /*
   * The ELF magic corresponds to an RSH instruction with an offset,
   * which is invalid.
   */
  bool elf = code_len >= SELFMAG && !memcmp(code, ELFMAG, SELFMAG);

  char *errmsg;
  int rv;
  if (elf) {
    //printf("loading this elf %ld\n", code_len);
    rv = ubpf_load_elf(vm, code, code_len, &errmsg);
  } else {
    rv = ubpf_load(vm, code, code_len, &errmsg);
  }

  if (rv < 0) {
    fprintf(stderr, "Failed to load code: %s\n", errmsg);
    free(errmsg);
    ubpf_destroy(vm);
    return NULL;
  }

  return vm;
}
  
ubpf_jit_fn jit_compile(struct ubpf_vm *vm) {
    char *errmsg;
    ubpf_jit_fn fn;
    fn = ubpf_compile(vm, &errmsg);
    printf("jit compiled: %p\n", fn);
    if (fn == NULL) {
      fprintf(stderr, "Failed to compile: %s\n", errmsg);
      free(errmsg);
      return NULL;
    }

    return fn;
}

int run_bpf_code(struct ubpf_vm *vm, ubpf_jit_fn fn, struct xdp_md *ctx, size_t pkt_len, uint64_t *bpf_ret_val) {
  uint64_t ret = 0;

  if (fn != NULL) {
    *bpf_ret_val = fn(ctx, pkt_len);
  } else {
    ubpf_exec(vm, ctx, pkt_len, bpf_ret_val);
  }

  return ret;
}

int vm_size(void) {
  return ubpf_vm_size();
}

int dma_req_size(void) {
  return sizeof(dma_req_t);
}

} // namespace utils
} // namespace bess
