#ifndef __NAAM_H__
#define __NAAM_H__

#include <stdint.h>
#include <assert.h>
#include <linux/if_ether.h>
#include <netinet/ip.h>
#include <linux/udp.h>
#if (__x86_64__ || __aarch64__)
#include <stdio.h>
#endif

#include <ubpf.h>

#define NAAM_DEBUG 0

// Round up to the next power of 2
// src: https://graphics.stanford.edu/%7Eseander/bithacks.html#RoundUpPowerOf2
#define ROUNDUP2(V) \
({ \
 unsigned int v = (unsigned int)(V); \
 v--; \
 v |= v >> 1; \
 v |= v >> 2; \
 v |= v >> 4; \
 v |= v >> 8; \
 v |= v >> 16; \
 v++; \
 v; \
 })

//constants for use in the hashtable
//allow for less round trips and significant precalculation
#ifndef NUM_KEYS
#define NUM_KEYS 1000000
#endif
#ifndef KEY_NUM_BITS
#define KEY_NUM_BITS 20
#endif
#ifndef NUM_BUCKETS
#define NUM_BUCKETS 131072
#endif
#define BUCKET_SIZE 128
#define KEY_TYPE uint64_t
#define VAL_TYPE uint64_t
#define KEY_SIZE sizeof(KEY_TYPE)
#define VAL_SIZE sizeof(VAL_TYPE) 
#define CAS_TYPE uint32_t
#define HASH_SEED 2048U

// Port number for routing DPU atomic operations to host
#define HOST_ATOMIC_PORT 3163

// TODO(stutsman) Why does offsetof not work? It seems to generate a bad
// relocation type 10 due to compiling in a type of section that the ubpf
// loader doesn't support. This seems to work despite the UB.
#define off(type, field) (uintptr_t)&(((type*)0)->field)

struct vm_state {
    uint64_t regs[16];
    uint64_t stack[(UBPF_STACK_SIZE+7)/8];
    uint64_t pc;
    uint64_t pad0;
} __attribute__((packed, aligned(16)));

static_assert(sizeof(struct vm_state) == 656, "sizeof(struct vm_state) should be 656 bytes");

#define VM_STATE_SIZE sizeof(struct vm_state)

typedef uint64_t dma_addr_t;

typedef struct __attribute__((packed, aligned(16))) dma_req {
  dma_addr_t src_addr;
  dma_addr_t dst_addr;
  uint64_t size;
  uint8_t op;
  uint8_t status;
  uint32_t pad0;
} dma_req_t;

#define DMA_REQ_SIZE sizeof(struct dma_req)
#define RET_DMA_HELPER 0xffffffffffffffff
#define RET_DMA_SUCCESS 0
#define RET_DMA_FAILURE 1

#define DMA_ADDR(app_id, mem_id, offset) \
  (dma_addr_t)((((uint64_t)(app_id)) << 56) | (((uint64_t)(mem_id)) << 48) | ((uint64_t)(offset)))

#define DMA_ADDR_APP_ID(addr) \
  ((uint8_t)(addr >> 56))

#define DMA_ADDR_MEM_ID(addr) \
  ((uint8_t)(addr >> 48))

#define DMA_ADDR_OFFSET(addr) \
  ((uint64_t)(addr & 0x0000ffffffffffff))

#define CAS_PACK(oldval, newval) ((uint64_t)oldval << 32 | (uint64_t)newval)

#define CAS_UNPACK_OLDVAL(val) (val >> 32)

#define CAS_UNPACK_NEWVAL(val) (val & 0x00000000ffffffff)

typedef struct __attribute__((packed, aligned(16))) req_pkt {
  struct ethhdr eth;
  struct iphdr ip;
  struct udphdr udp;
  char pad0[6];
  struct vm_state state;
  dma_req_t dma_req;
  uint16_t param_size;
  uint64_t timestamp;
  uint64_t timestamp2;
  uint64_t nseq;
  uint8_t server_type;
  uint8_t func_id;
  uint8_t data[0];
} req_pkt_t;

#define SECTION(name) __attribute__((section(name), used))

// data and data_end is modified from uint32_t to uint64_t
struct xdp_md {
    uint64_t data;
    uint64_t data_end;
    uint32_t data_meta;
    uint32_t _1;
    uint32_t _2;
    uint32_t _3;
};

#define MAX_PKT_SZ 1500

#define APP_REGION_OFFSET(pkt) off(req_pkt_t, data)

#define APP_REGION_PTR(pkt) ((void *)(&((req_pkt_t *)pkt)->data))

enum { SERVER_TYPE_DPU, SERVER_TYPE_HOST };
enum { REQT_GET, REQT_SET };
enum { DMA_READ_OP, DMA_WRITE_OP, DMA_CAS_OP, DMA_FAA_OP };

#if (__x86_64__ || __aarch64__)
uint64_t dma_read(uint64_t *pkt);
uint64_t dma_write(uint64_t *pkt);
uint64_t bpf_yield(uint64_t *pkt, uint64_t *pkt_len);
uint64_t bpf_memcpy(uint64_t *dest, uint64_t *src, uint64_t size);
uint64_t bpf_memcmp(uint64_t *dest, uint64_t *src, uint64_t length);
#elif __bpf__
static long (*bpf_trace_printk)(const char *fmt, __u32 fmt_size, ...) = (void *) 6;
static uint64_t(*bpf_ktime_get_ns)(void) = (void *) 5;
static uint64_t(*dma_read)(uint64_t *, uint64_t, uint64_t) = (void *) 186;
static uint64_t(*dma_write)(uint64_t *, uint64_t, uint64_t) = (void *) 187;
static uint64_t(*bpf_hash)(const void *, uint64_t, void *, uint64_t, uint64_t) = (void *)188;
static uint64_t(*bpf_memcpy)(uint64_t *, uint64_t *, uint64_t) = (void *)189;
static uint64_t(*bpf_yield)(uint64_t *, uint64_t) = (void *)7;
static uint64_t(*bpf_memcmp)(uint64_t *, uint64_t *, uint64_t) = (void *)8;
static void(*dump_hex)(const uint64_t *, const int) = (void *)10;
static uint64_t(*bpf_rand)(void) = (void *)11;
#endif

// This hack is needed because the ubpf ELF loader doesn't support
// rodata sections to hold string literals. This trick forces those
// strings into the code at the cost of some extra runtime overhead and
// binary bloat.
#if (__x86_64__ || __aarch64__)
#define printm(fmt, ...) printf(fmt, ##__VA_ARGS__);
#elif __bpf__
#define printk(fmt, ...) \
  ({ \
      char ____fmt[] = fmt; \
      bpf_trace_printk(____fmt, sizeof(____fmt), ##__VA_ARGS__); \
  })
#endif

#if (__x86_64__ || __aarch64__)
#define DMA_READ(pkt, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_READ_OP; \
      dma_read(pkt); \
  })
#elif __bpf__
#define DMA_READ(pkt, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_READ_OP; \
      uint64_t __ret = dma_read(pkt, MAX_PKT_SZ, 0); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      __ret; \
  })
#endif

#define DMA_READ_TRUSTED(pkt, bitvect, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_READ_OP; \
      uint64_t __ret = dma_read(pkt, MAX_PKT_SZ, bitvect); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      __ret; \
  })

#if (__x86_64__ || __aarch64__)
#define DMA_WRITE(pkt, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_WRITE_OP; \
      dma_write((pkt)); \
  })
#elif __bpf__
#define DMA_WRITE(pkt, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_WRITE_OP; \
      uint64_t __ret = dma_write(pkt, MAX_PKT_SZ, 0); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      __ret; \
  })
#endif

#define DMA_WRITE_TRUSTED(pkt, bitvect, daddr, saddr, len) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.src_addr = (saddr); \
      req->dma_req.dst_addr = (daddr); \
      req->dma_req.size = (len); \
      req->dma_req.op = DMA_WRITE_OP; \
      uint64_t __ret = dma_write(pkt, MAX_PKT_SZ, bitvect); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      __ret; \
  })

// TODO: native(x86_64, aarch64) function is incomplete
//
// Size field in dma_req is used to pack oldval and newval.
// oldval is in the upper 32 bits and newval is in the lower 32 bits.
//
// Return value of cas operation is returned in the size field as lower 32 bits.
#if (__x86_64__ || __aarch64__)
#define DMA_CAS(pkt, addr, oldval, newval) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.dst_addr = (addr); \
      req->dma_req.size = CAS_PACK(oldval, newval); \
      req->dma_req.op = DMA_CAS_OP; \
      dma_read((pkt)); \
  })
#elif __bpf__
#define DMA_CAS(pkt, addr, oldval, newval) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.dst_addr = (addr); \
      req->dma_req.size = CAS_PACK(oldval, newval); \
      req->dma_req.op = DMA_CAS_OP; \
      uint64_t __ret = dma_read(pkt, MAX_PKT_SZ, 0); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      (CAS_TYPE)CAS_UNPACK_NEWVAL(req->dma_req.size); \
  })
#endif

// TODO: native(x86_64, aarch64) function is incomplete
//
// Size field in dma_req is used keep parameter for fetch and add.
//
// Return value of faa operation is returned in the size field.
#if (__x86_64__ || __aarch64__)
#define DMA_FAA(pkt, addr, val) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.dst_addr = (addr); \
      req->dma_req.size = (val); \
      req->dma_req.op = DMA_FAA_OP; \
      dma_read((pkt)); \
  })
#elif __bpf__
#define DMA_FAA(pkt, addr, val) \
  ({ \
      req_pkt_t* req = (req_pkt_t*)(pkt); \
      req->dma_req.dst_addr = (addr); \
      req->dma_req.size = (val); \
      req->dma_req.op = DMA_FAA_OP; \
      uint64_t __ret = dma_read(pkt, MAX_PKT_SZ, 0); \
      if (__ret == RET_DMA_HELPER) \
        return __ret; \
      req->dma_req.size; \
  })
#endif

#define DPrintf(fmt, ...) \
  do { \
    if (NAAM_DEBUG) \
      printk(fmt, ##__VA_ARGS__); \
  } while (0)

#if __bpf__
inline void send_reply(void *pkt) {
  req_pkt_t *req = (req_pkt_t *)pkt;

  unsigned char eth_src[ETH_ALEN];
  uint32_t ip_src = req->ip.saddr;
  uint16_t udp_src = req->udp.source;

  // Swap eth addresses
  bpf_memcpy((uint64_t *)eth_src, (uint64_t *)req->eth.h_source, ETH_ALEN);
  bpf_memcpy((uint64_t *)req->eth.h_source, (uint64_t *)req->eth.h_dest, ETH_ALEN);
  bpf_memcpy((uint64_t *)req->eth.h_dest, (uint64_t *)eth_src, ETH_ALEN);
  
  // Swap IP addresses
  req->ip.saddr = req->ip.daddr;
  req->ip.daddr = ip_src;

  // Swap UDP ports
  req->udp.source = req->udp.dest;
  req->udp.dest = udp_src;
}
#endif

// Data structure for the linked list
struct __attribute__((__packed__)) llnode {
	int val;
	uint32_t offset;
};

#define TREE_ORDER   20
#define BPT_NUM_KEYS     (TREE_ORDER * 2)
#define BPT_NUM_OFFSETS  (TREE_ORDER * 2 + 1)

// Data structures for bplus tree
typedef struct __attribute__((packed)) Node {
    uint64_t   keys[BPT_NUM_KEYS];        // stored keys
    uint64_t   children[BPT_NUM_OFFSETS]; // offset to child nodes
    uint64_t   n;                     // current key count
    uint8_t    leaf;                  // 1 if leaf
    uint64_t   next;                  // offset to sibling for leaves
} Node;

typedef struct __attribute__((packed)) bpt_search_req {
  uint64_t key;
} bpt_search_req_t;

// Status of the hashtable operations
typedef enum _HT_STATUS {
  HT_SUCCESS = 0,
  HT_ERR_FATAL,
  HT_ERR_LOCKED,
  HT_ERR_FULL,
  HT_ERR_HASH_NOT_MATCHED,
  HT_ERR_KEY_NOT_FOUND,
  HT_ERR_VERSION_UPDATE,
  HT_ERR_DUPLICATE,   /* write rejected as duplicate (nseq already applied) */
} HT_STATUS;

#endif
