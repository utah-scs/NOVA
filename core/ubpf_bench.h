#ifndef __UBPF_BENCH_H__
#define __UBPF_BENCH_H__

// Bytes to read/write for dma read/write op
#define DMA_RW_SIZE 16

// 4 bytes in the packet buffer to keep
// number of linked list nodes to walk
#define BUF_SIZE_LLIST 4

// struct for hashtable entries
typedef struct __attribute__((__packed__)) ht_entry {
	uint8_t cmd;
	uint64_t key;
	uint64_t value;
} ht_entry_t;

typedef struct __attribute__((packed)) get_set_entry {
  uint8_t op_type;
  uint64_t mr_offset;
  uint64_t data_size;
} get_set_entry_t;

#endif
