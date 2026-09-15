// Headers for this file
#include "dma.h"
#include "../utils/dma_common.h"

// Standard headers
#include <inttypes.h>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <string>
#include <algorithm>
#include <random>
#include <fstream>

// DOCA headers
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dma.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "../../deps/util/murmur.c"

#define UNUSED __attribute__((unused))

#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

DOCA_LOG_REGISTER(DMA_MODULE)

using bess::utils::be32_t;

#define MEHCACHED_ITEMS_PER_BUCKET 15
#define MEHCACHED_SINGLE_ALLOC

#define MEHCACHED_ROUNDUP8(x) (((x) + 7UL) & (~7UL))

struct mehcached_bucket
{
    uint32_t version;   // XXX: is uint32_t wide enough?
    //uint32_t next_extra_bucket_index;   // 1-base; 0 = no extra bucket
    uint64_t item_vec[MEHCACHED_ITEMS_PER_BUCKET];

    // 16: tag (1-base)
    //  8: alloc id
    // 40: item offset
    // item == 0: empty item

    #define MEHCACHED_TAG_MASK (((uint64_t)1 << 16) - 1)
    #define MEHCACHED_TAG(item_vec) ((item_vec) >> 48)

#ifndef MEHCACHED_SINGLE_ALLOC
    #define MEHCACHED_ALLOC_ID_MASK (((uint64_t)1 << 8) - 1)
    #define MEHCACHED_ALLOC_ID(item_vec) (((item_vec) >> 40) & MEHCACHED_ALLOC_ID_MASK)
#else
    #define MEHCACHED_ALLOC_ID(item_vec) (0LU)
#endif

#ifndef MEHCACHED_SINGLE_ALLOC
    #define MEHCACHED_ITEM_OFFSET_MASK (((uint64_t)1 << 40) - 1)
#else
    #define MEHCACHED_ITEM_OFFSET_MASK (((uint64_t)1 << 48) - 1)
#endif
    #define MEHCACHED_ITEM_OFFSET(item_vec) ((item_vec) & MEHCACHED_ITEM_OFFSET_MASK)

#ifndef MEHCACHED_SINGLE_ALLOC
    #define MEHCACHED_ITEM_VEC(tag, alloc_id, item_offset) (((uint64_t)(tag) << 48) | ((uint64_t)(alloc_id) << 40) | (uint64_t)(item_offset))
#else
    #define MEHCACHED_ITEM_VEC(tag, item_offset) (((uint64_t)(tag) << 48) | (uint64_t)(item_offset))
#endif
};

struct mehcached_item
{
    uint64_t key_hash;
    KEY_TYPE key;
    VAL_TYPE val;
    uint64_t nseq;    /* nseq of last successfully applied write; 0 = never written */
};
  
struct rand_state {
  uint64_t a;
};

struct bpt_state {
  void *mem;
  uint64_t offset_free;
  Node *root;
};

// convert pointer to offset (x - base)
#define OFFSET(x, base) ((uint64_t)(x) - (uint64_t)(base))

// convert offset to pointer x + y
#define PTR(x, base) ((Node *)((char *)base + (x)))

 const Commands DMA::cmds = {
     /* This command adds a new memory region */
     {"add_memory", "DMAArg",
       MODULE_CMD_FUNC(&DMA::AddMemory), Command::THREAD_UNSAFE},
     /* This command deletes all the memory regions */
     {"clear_memory", "EmptyArg",
       MODULE_CMD_FUNC(&DMA::ClearMemory), Command::THREAD_UNSAFE},
     /* 
      * This command returns number of bytes transfarred through
      * doca dma
      */
     {"get_dma_stats", "EmptyArg",
       MODULE_CMD_FUNC(&DMA::GetDMAStats), Command::THREAD_SAFE}
};

CommandResponse
DMA::Init(const dma::dmatrans::pb::DMAArg &arg) {
  int ret;
  num_bytes_dma = 0;
  mem_type = (MemType)arg.memtype();

  // Resolve hashtable sizing: use proto arg if provided, else compiled-in default
  num_keys = arg.num_keys() > 0 ? arg.num_keys() : NUM_KEYS;
  uint32_t n = (uint32_t)(num_keys / MEHCACHED_ITEMS_PER_BUCKET);
  n--;
  n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16;
  num_buckets = n + 1;
  
  LOG(INFO) << "BPF VM info:\n";
  LOG(INFO) << "VM state size: " << VM_STATE_SIZE << "\n";
  LOG(INFO) << "BPF stack size: " << UBPF_STACK_SIZE << "\n";
  LOG(INFO) << "DMA descriptor size: " << DMA_REQ_SIZE << "\n";
  
  // If no bluefield pcie address is provide
  // then host only setup is assumed
  if (arg.pci_addr().empty() == true) {
    doca_dma = false;
    pci_addr = 0;
  }
  else {
    doca_dma = true;
    pci_addr = arg.pci_addr().c_str();
  }

  if (mem_type == DMA::LOCAL) {
    LOG(INFO) << "MEMTYPE: " << arg.memtype();
    LOG(INFO) << "Host PCIe address: " << pci_addr;
    copier.reset(new MemCopier(pci_addr, "192.168.100.2", 10001));

    if (arg.id() != 0) {
      std::string ds = (arg.structure().empty() == true ? "plain" : arg.structure());
      ret = RegisterMemory(arg.id(), arg.size(), ds, arg.init());

      if (ret != 0) 
        return CommandFailure(EINVAL, "Failed creating memory region");
    region_length[arg.id()] = arg.size();
    }
  }
  else if (mem_type == DMA::LDMA) {
    DOCA_LOG_INFO("Initializing DOCACopier");
    DOCA_LOG_INFO("DPU PCIe address %s", pci_addr);
    copier.reset(new DOCACopier(pci_addr));

    if (copier->ReceiveMemInfo(arg.id()) != DOCA_SUCCESS)
        return CommandFailure(EINVAL, "Failed creating memory region");
    region_length[arg.id()] = arg.size();
  }
  
  return CommandSuccess();
}

void DMA::DeInit() {
  LOG(INFO) << "DMA: Destruction on process\n";
  for (auto &elem: shmInfo) {
    uint32_t id = elem.first;
    auto &info =  elem.second;

    LOG(INFO) << "DMA: Removing shm file -> " << std::get<0>(info).c_str()
      << "\n";

    // unmap memory region
    munmap(memory_regions[id], std::get<2>(info));

    if (std::get<1>(info) != -1) {
      close(std::get<1>(info));
      // hugetlbfs paths are absolute ("/mnt/huge/..."); shm names are bare.
      // unlink may fail if another DMA instance already removed the file — ignore.
      const std::string &name = std::get<0>(info);
      if (name[0] == '/')
        (void)unlink(name.c_str());
      else
        (void)shm_unlink(name.c_str());
    }
  }
}

/*
 * Add a new memory region
 */
CommandResponse
DMA::AddMemory(const dma::dmatrans::pb::DMAArg &arg) {
  int ret;
  std::string ds = (arg.structure().empty() == true ? "plain" : arg.structure());

  if (ds != "plain")
    LOG(WARNING) << "Data structure other than palin isn't supported for add_memory\n";

  ret = RegisterMemory(arg.id(), arg.size(), "plain", arg.init());

  if (ret != 0) 
    return CommandFailure(EINVAL, "Failed to create memory region");
  
  return CommandSuccess();
}

/*
 * Delete all the memory regions
 */
CommandResponse
DMA::ClearMemory(const bess::pb::EmptyArg &) {
  DeInit();

  return CommandSuccess();
}

/*
 * Get total bytes move through DOCA DMA
 */
CommandResponse
DMA::GetDMAStats(const bess::pb::EmptyArg &) {
  dma::dmatrans::pb::DMAStatsResopnse r;
  LOG(INFO) << "DMA bytes: " << num_bytes_dma << "\n";
  r.set_num_bytes(num_bytes_dma);

  return CommandSuccess(r);
}

int DMA::SetupLinkedList(uint32_t region_id, uint64_t size) {
  // Create 32 nodes
  std::vector<struct llnode> list;
  size_t node_size = sizeof(struct llnode);
  void *memory_region = memory_regions[region_id];

  // Check if there is enough room
  // for 32 nodes
  if (size <= (32 * node_size)) {
    LOG(ERROR) << "Not enough memory for 32 nodes in the linked list\n";
    return -1;
  }

  for (uint32_t i = 0; i < 32; i++) {
    struct llnode l = {5, i*8};
    list.push_back(l);
  }

  // Random sort the list to simulate real linked list traversal
  unsigned seed = 100;
  shuffle(list.begin(), list.end(), std::default_random_engine(seed));
  
  // Debug print
  int i = 0;
  LOG(INFO) << "LINKED LIST OFFSETS" << "\n";
  for (auto it = list.begin(); it != list.end(); it++) {
    LOG(INFO) << "Index: " << i * 8 << "\tOffset: " << it->offset <<
      "\tValue: " << it->val << "\n";
    i++;
  }

  // Copy the list into shared memory
  memcpy(memory_region, static_cast<void *>(list.data()), node_size * list.size());

  // Debug print
  LOG(INFO) << "LINKED LIST FETCHED FROM SHARED MEMORY" << "\n";
  struct llnode *ll = static_cast<struct llnode *>(memory_region);
  for (uint32_t i = 0; i < 32; i++) {
    LOG(INFO) << "Index: " << i * 8 << "\tOffset: " << ll[i].offset <<
      "\tValue: " << ll[i].val << "\n";
  }

  // Debug print
  // Walk the linked list
  LOG(INFO) << "LINKED LIST WALK" << "\n";
  uint64_t offset = 0;
  for (uint32_t i = 0; i < 32; i++) {
    struct llnode *node = static_cast<struct llnode *>((void *)((char *)memory_region + offset));
    LOG(INFO) << "Node: " << i << "\tOffset: " << node->offset <<
      "\tValue: " << node->val << "\n";
    offset = node->offset;
  }

  return 0;
}

// 64 bit random number generator
uint64_t DMA::GetRandomNumber(struct rand_state *state) {
	uint64_t x = state->a;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return state->a = x;
}

void PrintBuckets(void *memreg, uint32_t nb) {
    for (uint32_t i = 0; i < nb; i++) {
      struct mehcached_bucket *bucket = (struct mehcached_bucket *)((char *)memreg + sizeof(uint64_t) + sizeof(struct mehcached_bucket) * i);

      LOG(INFO) << "\nBUCKET -> " << i << "\n"
        << "\tversion: " << bucket->version << "\n"
        << "\titem vect 0: " << bucket->item_vec[0] << "\n"
        << "\titem vect 1: " << bucket->item_vec[1] << "\n"
        << "\titem vect 2: " << bucket->item_vec[2] << "\n"
        << "\titem vect 3: " << bucket->item_vec[3] << "\n"
        << "\titem vect 4: " << bucket->item_vec[4] << "\n"
        << "\titem vect 5: " << bucket->item_vec[5] << "\n"
        << "\titem vect 6: " << bucket->item_vec[6] << "\n"
        << "\titem vect 7: " << bucket->item_vec[7] << "\n"
        << "\titem vect 8: " << bucket->item_vec[8] << "\n"
        << "\titem vect 9: " << bucket->item_vec[9] << "\n"
        << "\titem vect 10: " << bucket->item_vec[10] << "\n"
        << "\titem vect 11: " << bucket->item_vec[11] << "\n"
        << "\titem vect 12: " << bucket->item_vec[12] << "\n"
        << "\titem vect 13: " << bucket->item_vec[13] << "\n"
        << "\titem vect 14: " << bucket->item_vec[14] << "\n\n";

      for (uint32_t j = 0; j < 15; j++) {
        uint64_t item_offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * nb + MEHCACHED_ITEM_OFFSET(bucket->item_vec[j]);
        struct mehcached_item *item = (struct mehcached_item *)((char *)memreg + item_offset);
        LOG(INFO) << "ITEM INDEX -> " << j
          << "\tkey: " << item->key
          << "\tvalue: " << item->val << "\n";
      }
    }
}

int DMA::SetupHashTable(uint32_t region_id) {
    void *memreg = memory_regions[region_id];

    LOG(INFO) << "HT: Size of each bucket " << sizeof(struct mehcached_bucket)
      << "\n";
    LOG(INFO) << "HT: Number of buckets -> " << num_buckets << "\n";
    LOG(INFO) << "HT: Number of keys -> " << num_keys << "\n";

    *(uint64_t*)memreg = sizeof(uint64_t) + num_buckets * BUCKET_SIZE;

    LOG(INFO) << "HT: Allocated -> " << *(uint64_t*)memreg << " bytes\n";

    // Initialize random number generator
    struct rand_state *r_state;
		r_state = (struct rand_state *)malloc(sizeof(struct rand_state));
		r_state->a = time(NULL);

    // Generate kv pair and pre-fill the bucket
    for (uint64_t i = 0; i < num_keys; i++) {
      uint64_t key = GetRandomNumber(r_state) & (((uint64_t)1 << KEY_NUM_BITS) - 1);
      uint64_t val = key + 1;

      uint64_t key_hash;
      MurmurHash3_x86_128(&key, sizeof(key_hash), HASH_SEED, &key_hash);

      // Create item
      struct mehcached_item item;
      item.key = key;
      item.val = val;
      item.key_hash = key_hash;
      item.nseq = 0;    /* 0 = never written; client nseqs start at PKT_SEQ_START=1 */

      // Calculate the bucket index and tag from the key hash
      uint32_t bucket_index = (uint32_t)(key_hash >> 16) & (num_buckets - 1);
      //uint32_t bucket_index = (uint32_t)(key_hash >> 16) & 131071U;
      uint16_t tag = (uint16_t)(key_hash & MEHCACHED_TAG_MASK);
      uint64_t bucket_offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index;

      struct mehcached_bucket *current_bucket = (struct mehcached_bucket *)((char *)memreg + bucket_offset);
      
      int ret = MEHCACHED_ITEMS_PER_BUCKET + 1;

      int item_index = 0;
      uint64_t item_vec = 0;
      for (item_index = 0; item_index < MEHCACHED_ITEMS_PER_BUCKET; item_index++)
      {
        //insertions are always in order, removal of any kind is not supported
        //so if a blank entry is foundbefore a match the key should just be inserted
        if (current_bucket->item_vec[item_index] == 0)
          break;
        if (MEHCACHED_TAG(current_bucket->item_vec[item_index]) == tag){
          ret = item_index;
          item_vec = current_bucket->item_vec[item_index];
          break;
        }
      }
      
      // Bucket full
      if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1 && item_index == MEHCACHED_ITEMS_PER_BUCKET){
        LOG(INFO) << "HT: BUCKET FULL INDEX -> " << bucket_index << "\n";
        continue;
      }
      
      // Allocate space if needed
      if (ret == MEHCACHED_ITEMS_PER_BUCKET + 1) {
        int alloc_size = sizeof(struct mehcached_item);
        uint64_t item_offset = *(uint64_t *)memreg + alloc_size;
        *(uint64_t *)memreg = item_offset;
        item_vec = MEHCACHED_ITEM_VEC(tag, item_offset);

        // Write item vector in the bucket
        uint64_t vec_offset = MEHCACHED_ROUNDUP8(sizeof(uint64_t) + sizeof(struct mehcached_bucket) * bucket_index + sizeof(current_bucket->version) + sizeof(uint64_t) * item_index);
        memcpy((char *)memreg + vec_offset, &item_vec, sizeof(item_vec));
      } 
      
      // Write item
      uint64_t item_offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * num_buckets + MEHCACHED_ITEM_OFFSET(item_vec);
      memcpy((char *)memreg + item_offset, &item, sizeof(item));
    }
    
    // Print all the buckets for debugging
    //PrintBuckets(memreg, num_buckets);

    return 0;
}

Node *bpt_allocate_node(struct bpt_state *state) {
  Node *newNode = (Node *)((char *)state->mem + state->offset_free);
  state->offset_free += sizeof(Node);

  return newNode;
}

// allocate and initialize a new node
Node *bpt_create_node(struct bpt_state *state, uint8_t leaf) {
    Node *x = bpt_allocate_node(state);
    
    x->n     = 0;
    x->leaf  = leaf;
    x->next  = 0;
    
    return x;
}

// search for key in the B+ tree; returns 1 if found, 0 otherwise
int bpt_search(struct bpt_state *state, uint64_t key) {
    printf("searching for %lu\n", key);
    if (!state->root) return 0;
    Node *c = state->root;
    printf("offset of root node: %lu\n", OFFSET(c, state->mem));
    printf("Number of keys in root node: %lu\n", c->n);
    
    // descend to leaf
    while (!c->leaf) {
        int i = 0;
        // search for the first key greater than or equal to key
        while (i < (int)c->n && key >= c->keys[i]) i++;
        printf("offset of child node: %lu\n", c->children[i]);
        c = PTR(c->children[i], state->mem);
        printf("offset of child node: %lu\n", OFFSET(c, state->mem));
    }
    
    // linear scan in leaf
    for (int i = 0; i < (int)c->n; i++)
        if (c->keys[i] == key) {
          printf("key index in node: %d\n", i);
          return 1;
        }
    
    return 0;
}

// split child at index i
void bpt_split_child(struct bpt_state *state, Node *parent, int i, Node *child) {
    // create new node, same leaf status as child
    Node *newNode = bpt_create_node(state, child->leaf);
    
    // new node gets TREE_ORDER keys from child
    newNode->n = TREE_ORDER;
    
    for (int j = 0; j < TREE_ORDER; j++)
        newNode->keys[j] = child->keys[j + TREE_ORDER];
    
    if (!child->leaf) {
        // internal: move TREE_ORDER + 1 children
        for (int j = 0; j < TREE_ORDER + 1; j++)
            newNode->children[j] = child->children[j + TREE_ORDER];
    } else {
        // leaf: hook into leaf‐chain
        newNode->next    = child->next;
        child->next    = OFFSET(newNode, state->mem);
    }

    // shrink child
    child->n = TREE_ORDER + 1;

    // make room in parent
    for (int j = parent->n; j > i; j--)
        parent->children[j + 1] = parent->children[j];
    
    // Index i contains old node, place new node in i + 1
    parent->children[i + 1] = OFFSET(newNode, state->mem);
    
    for (int j = parent->n - 1; j >= i; j--)
        parent->keys[j+1] = parent->keys[j];
    
    // median key up into parent
    parent->keys[i] = child->keys[TREE_ORDER];
    parent->n++;
}

// insert key into non‐full node
void bpt_insert_non_full(struct bpt_state *state, Node *node, uint64_t key) {
    // index of the last key in the node
    int i = node->n - 1;
    if (node->leaf) {
        // shift to make room
        while (i >= 0 && key < node->keys[i]) {
            node->keys[i+1] = node->keys[i];
            i--;
        }
        node->keys[i+1] = key;
        node->n++;
    } else {
        // descend to correct child
        while (i >= 0 && key < node->keys[i]) i--;
        i++;
        
        Node *c = PTR(node->children[i], state->mem);
        if (c->n == BPT_NUM_KEYS) {
            // child full → split
            bpt_split_child(state, node, i, c);
            // decide which of the two to descend into
            if (key > node->keys[i]) i++;
        }

        // recursively insert into child
        bpt_insert_non_full(state, PTR(node->children[i], state->mem), key);
    }
}

// top‐level insert
void bpt_insert(struct bpt_state *state, uint64_t key) {
    if (!state->root) {
        state->root = bpt_create_node(state, 1);
        state->root->keys[0] = key;
        state->root->n       = 1;
        return;
    }
    if (state->root->n == BPT_NUM_KEYS) {
        // root is full → grow tree in height
        Node *s = bpt_create_node(state, 0);
        s->children[0] = OFFSET(state->root, state->mem);
        bpt_split_child(state, s, 0, state->root);
        state->root = s;
        bpt_insert_non_full(state, s, key);
    } else {
        bpt_insert_non_full(state, state->root, key);
    }
}

Node *bpt_root(struct bpt_state *state) {
  return state->root;
}

uint64_t bpt_rootoffset(struct bpt_state *state) {
  return OFFSET(state->root, state->mem);
}

// simple traversal for debugging: print the first key of each node by level
void bpt_dump(struct bpt_state *state, Node *r, int depth) {
    if (!r) return;
    printf("%*s[", depth*2, "");
    for (int i = 0; i < (int)r->n; i++) {
        printf("%lu", r->keys[i]);
        if (i+1 < (int)r->n) printf(",");
    }
    printf("]\n");
    if (!r->leaf) {
        for (int i = 0; i <= (int)r->n; i++)
            bpt_dump(state, PTR(r->children[i], state->mem), depth+1);
    }
}

void bpt_dump_node(Node *r) {
    if (!r) return;
    printf("node: %p\n", r);
    printf("n: %lu\n", r->n);
    printf("leaf: %d\n", r->leaf);
    printf("next: %lu\n", r->next);
    for (int i = 0; i < (int)r->n; i++) {
        printf("%lu ", r->keys[i]);
    }
    printf("\n");
}

int bpt_num_levels(struct bpt_state *state) {
    if (!state->root) 
        return 0;

    int levels = 0;
    Node *node = state->root;
    // descend until a leaf
    while (true) {
        levels++;
        if (node->leaf)
            break;
        // follow first child
        node = PTR(node->children[0], state->mem);
    }
    return levels;
}

void bpt_init(struct bpt_state *state, void *memreg) {
  state->mem = memreg;
  state->offset_free = 0;
  state->root = NULL;
}

int DMA::SetupBPTree(uint32_t region_id) {
  void *memreg = memory_regions[region_id];
  struct bpt_state *state = (struct bpt_state *)malloc(sizeof(struct bpt_state));

  bpt_init(state, memreg);
  
  srand(3163);

  for (int i = 0; i < 10000000; i++) {
      uint64_t key = rand() % 10000000;
      bpt_insert(state, key);
  }

  // uint64_t data[] = {10, 20, 5, 6, 12, 30, 7, 17};
  // for (int i = 0; i < (int)(sizeof(data)/sizeof(*data)); i++)
  //     bpt_insert(state, data[i]);

  // Node *root = bpt_root(state);
  // bpt_dump(state, root, 0);
  //
  // printf("search 6 → %s\n", bpt_search(state, 6) ? "found" : "not found");
  // printf("search 15 → %s\n", bpt_search(state, 15)? "found" : "not found");

  printf("offset root node: %lu\n", bpt_rootoffset(state));
  printf("num levels: %d\n", bpt_num_levels(state)); 
  printf("size of node: %lu\n", sizeof(Node));
  printf("size of the tree: %lu bytes\n", state->offset_free);
  // std::cout << "B+ tree setup: " << region_id << "\n";

  return 0;
}

int DMA::RegisterMemory(uint32_t region_id, uint64_t region_size, std::string ds, bool initialize) {
  std::string shm_id;
  int ret;
  int fd;
  
  LOG(INFO) << "RegisterMemory: " << region_id << "\n";

  if (region_id == 0) {
    LOG(ERROR) << "memory region: id 0 is reserved for packet buffer\n";
    return -1;
  }
  
  if (region_size == 0) {
    LOG(ERROR) << "memory region: size 0 is invalid\n";
    return -1;
  }

  LOG(INFO) << "Creating new memory region [ ID: " 
    << region_id << ", SIZE: " << region_size
    << ", STRUCTURE: " << ds << " ]\n";
 
  shm_id = std::string("dma_shm_") + std::to_string(region_id);
  fd = -1;

  // Try hugetlbfs file-backed mmap for shareable 1GB hugepages.
  // Unlike MAP_ANONYMOUS, a named file on hugetlbfs can be opened by multiple
  // DMA instances so they all map the same physical pages — fixing the issue
  // where a second instance with init=false would get a fresh zero-filled mapping.
  // Requires hugetlbfs mounted at /mnt/huge and 1GB hugepages pre-reserved:
  //   mount -t hugetlbfs -o pagesize=1G none /mnt/huge
  //   echo 2 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
  std::string huge_path = std::string("/mnt/huge/") + shm_id;
  fd = open(huge_path.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd != -1) {
    ret = ftruncate(fd, region_size);
    if (ret == -1) {
      LOG(WARNING) << "memory region " << region_id
                   << ": ftruncate on hugetlbfs failed, falling back to 4K pages\n";
      close(fd);
      fd = -1;
    } else {
      memory_regions[region_id] = mmap(NULL, region_size,
          PROT_READ | PROT_WRITE,
          MAP_SHARED | MAP_HUGETLB | MAP_HUGE_1GB, fd, 0);
      if (memory_regions[region_id] == MAP_FAILED) {
        LOG(WARNING) << "memory region " << region_id
                     << ": hugetlbfs mmap failed, falling back to 4K pages\n";
        close(fd);
        fd = -1;
      } else {
        shm_id = huge_path;  // store full path so DeInit can unlink it
      }
    }
  } else {
    LOG(WARNING) << "memory region " << region_id
                 << ": /mnt/huge not available, falling back to 4K pages\n";
  }

  if (fd == -1) {
    shm_id = std::string("dma_shm_") + std::to_string(region_id);
    fd = shm_open(shm_id.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
      LOG(ERROR) << "memory region " << region_id << ": shm_open failed\n";
      return -1;
    }
    ret = ftruncate(fd, region_size);
    if (ret == -1) {
      LOG(ERROR) << "memory region " << region_id << ": ftruncate failed\n";
      close(fd);
      return -1;
    }
    memory_regions[region_id] = mmap(NULL, region_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  }

  if (memory_regions[region_id] == MAP_FAILED) {
    LOG(ERROR) << "memory region " << region_id << ": mmap failed\n";
    if (fd != -1) {
      close(fd);
      if (shm_id[0] == '/') unlink(shm_id.c_str());
      else shm_unlink(shm_id.c_str());
    }
    return -1;
  }

  region_length[region_id] = region_size;

  if (initialize)
    memset(memory_regions[region_id], 0, region_size);
  
  LOG(INFO) << "Memory region created [ ID: " 
    << region_id << ", SIZE: " << region_size
    << ", START: " << memory_regions[region_id]
    << ", END: " << static_cast<void *>((char *)memory_regions[region_id] + region_size)
    << ", STRUCTURE: " << ds << " ]\n";

  // Send memory region info to DPU
#if __x86_64__
  if (doca_dma == true) {
    LOG(INFO) << "Sending meminfo -> addr: " << (uint64_t)memory_regions[region_id]
      << "\tsize: " << region_size;
    copier->SendMemInfo(memory_regions[region_id], region_size, region_id);
  }
#endif

  // Insert shm file info
  insertSHMInfo(region_id, shm_id, fd, region_size);

  // Setup data structure if any provided
  if (ds == "llist") {
    if (initialize) {
      ret = SetupLinkedList(region_id, region_size);

      if (ret == -1) {
        LOG(ERROR) << "memory region " << region_id << ": Error creating linked list\n";
        return -1;
      }
    }
  } else if (ds == "hashtable"){
    uint64_t min_size = sizeof(uint64_t)
        + (uint64_t)num_buckets * BUCKET_SIZE
        + num_keys * sizeof(struct mehcached_item);
    if (region_size < min_size) {
      LOG(ERROR) << "Region too small for hashtable: need " << min_size
                 << " got " << region_size << "\n";
      return -1;
    }
    if (initialize) {
      LOG(INFO) << "RegisterMemory: Setting up hashtable\n";
      SetupHashTable(region_id);
    }
    else {
      LOG(INFO) << "RegisterMemory: Skip hashtable setup\n";
      LOG(INFO) << "HT: Allocated -> " << *(uint64_t *)memory_regions[region_id] << " bytes\n";
    }
  } else if (ds == "bptree") {
    if (initialize) {
      LOG(INFO) << "RegisterMemory: Setting up bptree\n";
      SetupBPTree(region_id);
    }
    else {
      LOG(INFO) << "RegisterMemory: Skip bptree setup\n";
    }
  } else if (ds != "plain") {
    LOG(ERROR) << "RegisterMemory: Unknown data structure\n";
    return -1;
  }
  
  return 0;
}

doca_error_t MemCopier::Init() {
  doca_error_t result;

  // Ignore doca in host only setup
  if (pci_addr == NULL) {
    DOCA_LOG_INFO("No PCI address provided, Assuming host without BlueField\n");
    return DOCA_SUCCESS;
  }

  result = open_doca_device_with_pci(pci_addr, &dma_jobs_is_supported, &state.dev);
  if (result != DOCA_SUCCESS) {
    return result;
  }

  /* Init all DOCA core objects */
  result = host_init_core_objects(&state);
  if (result != DOCA_SUCCESS) {
    host_destroy_core_objects(&state);
    return result;
  }

  return DOCA_SUCCESS;
}

void MemCopier::DeInit() {
  // Ignore doca in host only mode
  if (pci_addr != NULL)
    host_destroy_core_objects(&state);
}


doca_error_t MemCopier::SendMemInfo(void *memory_region, size_t memory_region_len,
    uint32_t memory_region_id) {

  char *export_data;
  size_t export_data_len;
  doca_error_t result;


  result = doca_mmap_set_memrange(state.mmap, memory_region, memory_region_len);
  if (result != DOCA_SUCCESS) {
    LOG(ERROR) << "SendMemInfo: doca_mmap_set_memrange failed: " << doca_error_get_descr(result);
    host_destroy_core_objects(&state);
    return result;
  }

  result = doca_mmap_set_permissions(state.mmap,
      DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_PCI_READ_WRITE);
  if (result != DOCA_SUCCESS) {
    LOG(ERROR) << "SendMemInfo: doca_mmap_set_permissions failed: " << doca_error_get_descr(result);
    host_destroy_core_objects(&state);
    return result;
  }

  result = doca_mmap_start(state.mmap);
  if (result != DOCA_SUCCESS) {
    LOG(ERROR) << "SendMemInfo: doca_mmap_start failed: " << doca_error_get_descr(result);
    host_destroy_core_objects(&state);
    return result;
  }

  result = doca_mmap_export_pci(state.mmap, state.dev, (const void **)&export_data, &export_data_len);
  if (result != DOCA_SUCCESS) {
    LOG(ERROR) << "SendMemInfo: doca_mmap_export_pci failed: " << doca_error_get_descr(result);
    host_destroy_core_objects(&state);
    return result;
  }

  /* Send memory region info through socket */
  //result = SendSocket((char *)export_data, export_data_len, (uint64_t)memory_region, memory_region_len);

  /* Export memory region information to file to register in DPU for DMA */
  result = ExportToFile((const char *)export_data, export_data_len, (uint64_t)memory_region,
              memory_region_len, memory_region_id);

  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Send host meminfo failed!");
    host_destroy_core_objects(&state);
    return result;
  }

  return DOCA_SUCCESS;
}

/* This will not be implemented for MemCopier */
doca_error_t MemCopier::ReceiveMemInfo(uint32_t memory_region_id) {
  (void)memory_region_id;

  return DOCA_SUCCESS;
}

std::unique_ptr<Copier::CopyHandle> MemCopier::StartDMA(void *dst, uint64_t dst_off,
    void *src, uint64_t src_off, uint64_t len, bool dst_is_pkt) {

  //(void)dst_is_pkt;
  memcpy((char *)dst + dst_off, (char *)src + src_off, len);

  if (NAAM_DEBUG) {

    // If size if 128 it's bucket read
    if (dst_is_pkt && len == 128) {
      struct mehcached_bucket *bucket = (struct mehcached_bucket *)((char *)dst + dst_off);
      LOG(INFO) << "BUCKET -> " << (src_off - 8) / 128 <<  ", OFF -> " << src_off << "\n"
        << "\tversion: " << bucket->version << "\n"
        << "\titem vect 0: " << bucket->item_vec[0] << "\n"
        << "\titem vect 1: " << bucket->item_vec[1] << "\n"
        << "\titem vect 2: " << bucket->item_vec[2] << "\n"
        << "\titem vect 3: " << bucket->item_vec[3] << "\n"
        << "\titem vect 4: " << bucket->item_vec[4] << "\n"
        << "\titem vect 5: " << bucket->item_vec[5] << "\n"
        << "\titem vect 6: " << bucket->item_vec[6] << "\n"
        << "\titem vect 7: " << bucket->item_vec[7] << "\n"
        << "\titem vect 8: " << bucket->item_vec[8] << "\n"
        << "\titem vect 9: " << bucket->item_vec[9] << "\n"
        << "\titem vect 10: " << bucket->item_vec[10] << "\n"
        << "\titem vect 11: " << bucket->item_vec[11] << "\n"
        << "\titem vect 12: " << bucket->item_vec[12] << "\n"
        << "\titem vect 13: " << bucket->item_vec[13] << "\n"
        << "\titem vect 14: " << bucket->item_vec[14] << "\n";
        
      for (uint32_t j = 0; j < 15; j++) {
          if (bucket->item_vec[j] == 0) {
            continue;
          }
          uint64_t _nb = (*(uint64_t*)src - sizeof(uint64_t)) / BUCKET_SIZE;
          uint64_t item_offset = sizeof(uint64_t) + sizeof(struct mehcached_bucket) * _nb + MEHCACHED_ITEM_OFFSET(bucket->item_vec[j]);
          struct mehcached_item *item = (struct mehcached_item *)((char *)src + item_offset);
          LOG(INFO) << "ITEM INDEX -> " << j
            << "\tkey: " << item->key
            << "\tvalue: " << item->val << "\n";
      }
    }

    // If read request for bpt bpt_dump_node
    if (dst_is_pkt && len == sizeof(Node)) {
      Node *node = (Node *)((char *)dst + dst_off);
      bpt_dump_node(node);
    }

    // Wrting item vect
    if (dst_is_pkt == false && len == 8) {
      LOG(INFO) << "DMA WRITE ITEM VECT PKT -> " << *(uint64_t *)((char *)src + src_off) << ", VECT MEM -> " << *(uint64_t *)((char *)dst + dst_off) << ", OFF -> " << dst_off << "\n";
    }

  }

  return std::make_unique<CopyHandle>();
}

doca_error_t MemCopier::SendSocket(const char *export_data, size_t export_data_len,
    uint64_t memory_region_addr, size_t memory_region_len) {

  struct sockaddr_in addr;
  struct timeval timeout = {
    .tv_sec = 5,
    .tv_usec = 0,
  };
  int sender_fd;
  int ret;
  //char ack_buffer[1024] = {0};
  //char exp_ack[] = "DMA operation on receiver node was completed";

  sender_fd = socket(AF_INET, SOCK_STREAM, 0);

  setsockopt(sender_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  addr.sin_addr.s_addr = inet_addr(ip);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (connect(sender_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    DOCA_LOG_ERR("Couldn't establish a connection to DPU");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }

  /* Send length of the exported data */
  DOCA_LOG_INFO("Sending exported data len: %lu", export_data_len);
  ret = write(sender_fd, &export_data_len, sizeof(export_data_len));
  if (ret != sizeof(export_data_len)) {
    DOCA_LOG_ERR("Failed to send exported data length to DPU");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }

  /* Send exported datat */
  ret = write(sender_fd, export_data, export_data_len);
  if (ret != (int)export_data_len) {
    DOCA_LOG_ERR("Failed to send exported data to DPU");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }
  
  /* Send src buffer VA */
  ret = write(sender_fd, &memory_region_addr, sizeof(memory_region_addr));
  if (ret != sizeof(memory_region_addr)) {
    DOCA_LOG_ERR("Failed to send memory region VA to DPU");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }
  
  /* Send src buffer length */
  ret = write(sender_fd, &memory_region_len, sizeof(memory_region_len));
  if (ret != sizeof(memory_region_len)) {
    DOCA_LOG_ERR("Failed to send memory region length to DPU");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }

  /* Waiting for DMA completion signal from receiver */
  /*
  DOCA_LOG_INFO("Waiting for DPU to acknowledge");
  if (recv(sender_fd, ack_buffer, sizeof(ack_buffer), 0) < 0) {
    DOCA_LOG_ERR("Failed to receive ack message");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }
  if (strcmp(exp_ack, ack_buffer)) {
    DOCA_LOG_ERR("Ack message is not correct");
    close(sender_fd);
    return DOCA_ERROR_UNEXPECTED;
  }
  DOCA_LOG_INFO("Ack message was received");
  */

  close(sender_fd);
  return DOCA_SUCCESS;
}

doca_error_t MemCopier::ExportToFile(const char *export_data, size_t export_data_len,
    uint64_t memory_region_addr, size_t memory_region_len, uint32_t memory_region_id) {

  FILE *fp;
  std::string fname_exportdata;
  std::string fname_meminfo;

  fname_exportdata = std::string("/tmp/exportdata_") + std::to_string(memory_region_id)
                         + std::string(".bin");

  fname_meminfo = std::string("/tmp/meminfo_") + std::to_string(memory_region_id)
                         + std::string(".txt");

  fp = fopen(fname_exportdata.c_str(), "wb");
  if (fp == NULL) {
    DOCA_LOG_ERR("Failed to create the DMA copy file");
    return DOCA_ERROR_IO_FAILED;
  }

  if (fwrite(export_data, 1, export_data_len, fp) != export_data_len) {
    DOCA_LOG_ERR("Failed to write all data into the file");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }

  fclose(fp);

  fp = fopen(fname_meminfo.c_str(), "w");
  if (fp == NULL) {
    DOCA_LOG_ERR("Failed to create the DMA copy file");
    return DOCA_ERROR_IO_FAILED;
  }

  fprintf(fp, "%" PRIu64 "\n", memory_region_addr);
  fprintf(fp, "%" PRIu64 "", memory_region_len);

  fclose(fp);

  return DOCA_SUCCESS;
}

void MemCopier::CopyHandle::await(void) {
}

DOCACopier::DOCACopier(const char *pci_addr): pci_addr(pci_addr) {
  if (Init() != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Initializing DOCACopier failed!");
    abort();
  }
}
/* Args passed to RegisterAllMemorySegments callback. */
struct RegisterSegArgs {
  struct doca_dev *dev;
  struct doca_mmap *first_mmap;  /* pre-created mmap from dpu_init_core_objects */
  bool first_seen;
  std::vector<DOCACopier::MemSeg> *segments;
};

/* Callback for registering all DPDK hugepage segments for DOCA DMA.
 * DOCA 3.3 only supports one memrange per mmap, so we create a separate
 * mmap for each segment beyond the first. */
int RegisterAllMemorySegments(__rte_unused const struct rte_memseg_list *msl,
    const rte_memseg *ms, void *args) {

  struct RegisterSegArgs *seg_args = (struct RegisterSegArgs *)args;
  doca_error_t result;
  struct doca_mmap *mmap_to_use;

  fprintf(stderr, "[RegisterAllMemorySegments] addr=%p len=%zu\n", ms->addr, ms->len);
  fflush(stderr);

  if (!seg_args->first_seen) {
    /* First segment: reuse the mmap already created by dpu_init_core_objects. */
    seg_args->first_seen = true;
    mmap_to_use = seg_args->first_mmap;
    result = doca_mmap_set_memrange(mmap_to_use, ms->addr, ms->len);
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "[RegisterAllMemorySegments] doca_mmap_set_memrange failed (seg0): %s\n",
              doca_error_get_descr(result));
      fflush(stderr);
      return -1;
    }
    /* Caller starts first_mmap after the walk completes. */
  } else {
    /* Additional segments: create a fresh mmap, fully configure and start it here. */
    result = doca_mmap_create(&mmap_to_use);
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "[RegisterAllMemorySegments] doca_mmap_create failed: %s\n",
              doca_error_get_descr(result));
      fflush(stderr);
      return -1;
    }
    result = doca_mmap_add_dev(mmap_to_use, seg_args->dev);
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "[RegisterAllMemorySegments] doca_mmap_add_dev failed: %s\n",
              doca_error_get_descr(result));
      fflush(stderr);
      doca_mmap_destroy(mmap_to_use);
      return -1;
    }
    result = doca_mmap_set_memrange(mmap_to_use, ms->addr, ms->len);
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "[RegisterAllMemorySegments] doca_mmap_set_memrange failed: %s\n",
              doca_error_get_descr(result));
      fflush(stderr);
      doca_mmap_destroy(mmap_to_use);
      return -1;
    }
    result = doca_mmap_start(mmap_to_use);
    if (result != DOCA_SUCCESS) {
      fprintf(stderr, "[RegisterAllMemorySegments] doca_mmap_start failed: %s\n",
              doca_error_get_descr(result));
      fflush(stderr);
      doca_mmap_destroy(mmap_to_use);
      return -1;
    }
  }

  seg_args->segments->push_back({ms->addr, ms->len, mmap_to_use});
  return 0;
}

static void dma_task_completed_cb(struct doca_dma_task_memcpy *dma_task,
                                   union doca_data task_user_data,
                                   union doca_data ctx_user_data)
{
  (void)ctx_user_data;
  DOCACopier::TaskCompletionData *data =
    (DOCACopier::TaskCompletionData *)task_user_data.ptr;
  data->result = DOCA_SUCCESS;
  data->completed = true;
  doca_task_free(doca_dma_task_memcpy_as_task(dma_task));
}

static void dma_task_error_cb(struct doca_dma_task_memcpy *dma_task,
                               union doca_data task_user_data,
                               union doca_data ctx_user_data)
{
  (void)ctx_user_data;
  DOCACopier::TaskCompletionData *data =
    (DOCACopier::TaskCompletionData *)task_user_data.ptr;
  struct doca_task *task = doca_dma_task_memcpy_as_task(dma_task);
  data->result = doca_task_get_status(task);
  data->completed = true;
  doca_task_free(task);
}

doca_error_t DOCACopier::Init() {
  doca_error_t result;

  DOCA_LOG_INFO("PCIe device to use: %s", pci_addr);
  result = open_doca_device_with_pci(pci_addr, &dma_jobs_is_supported, &state.dev);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Failed to open DOCA device: %s", doca_error_get_descr(result));
    return result;
  }

  result = doca_dma_create(state.dev, &dma_ctx);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to create DMA engine: %s", doca_error_get_descr(result));
    doca_dev_close(state.dev);
    state.dev = NULL;
    return result;
  }

  state.ctx = doca_dma_as_ctx(dma_ctx);

  result = doca_dma_task_memcpy_set_conf(dma_ctx, dma_task_completed_cb,
                                          dma_task_error_cb, MAX_DOCA_BUF);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to configure DMA tasks: %s", doca_error_get_descr(result));
    doca_dma_destroy(dma_ctx);
    doca_dev_close(state.dev);
    state.dev = NULL;
    return result;
  }

  result = dpu_init_core_objects(&state, MAX_DOCA_BUF);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("dpu_init_core_objects failed: %s", doca_error_get_descr(result));
    doca_dma_destroy(dma_ctx);
    doca_dev_close(state.dev);
    state.dev = NULL;
    return result;
  }

  result = doca_pe_connect_ctx(state.pe, state.ctx);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to connect PE to context: %s", doca_error_get_descr(result));
    dma_cleanup(&state, dma_ctx);
    return result;
  }

  result = doca_ctx_start(state.ctx);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("Unable to start DMA context: %s", doca_error_get_descr(result));
    dma_cleanup(&state, dma_ctx);
    return result;
  }

  DOCA_LOG_INFO("Registering DPDK memory segments for DMA");
  struct RegisterSegArgs seg_args = {
    .dev = state.dev,
    .first_mmap = state.mmap,
    .first_seen = false,
    .segments = &local_segs_,
  };
  if (rte_memseg_walk(RegisterAllMemorySegments, &seg_args) < 0)
    return DOCA_ERROR_INITIALIZATION;

  if (local_segs_.empty()) {
    DOCA_LOG_ERR("No DPDK memory segments found for DMA registration");
    return DOCA_ERROR_INITIALIZATION;
  }

  /* Start the first mmap (state.mmap) now that its memrange is set. */
  result = doca_mmap_start(state.mmap);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("doca_mmap_start failed for first segment: %s",
                 doca_error_get_descr(result));
    return result;
  }

  return DOCA_SUCCESS;
}

void DOCACopier::DeInit() {
  /* Destroy extra local mmaps (index 0 is state.mmap, cleaned up by dma_cleanup). */
  for (size_t i = 1; i < local_segs_.size(); i++) {
    if (local_segs_[i].mmap != nullptr) {
      doca_mmap_destroy(local_segs_[i].mmap);
      local_segs_[i].mmap = nullptr;
    }
  }
  local_segs_.clear();

  if (remote_mmap != nullptr) {
    if (doca_mmap_destroy(remote_mmap) != DOCA_SUCCESS)
      DOCA_LOG_ERR("Failed to destroy remote memory map");
    remote_mmap = nullptr;
  }

  if (dma_ctx != nullptr) {
    dma_cleanup(&state, dma_ctx);
    dma_ctx = nullptr;
  }
}


/* This will not be implemented for DOCACopier */
doca_error_t DOCACopier::SendMemInfo(void *memory_region, size_t memory_region_len,
                                   uint32_t memory_region_id) {

  (void)memory_region;
  (void)memory_region_len;
  (void)memory_region_id;

  return DOCA_SUCCESS;
}

/* TODO: Implement */
doca_error_t DOCACopier::ReceiveSocket(char *export_data, size_t *export_data_len,
    char **remote_addr, size_t *remote_addr_len) {

  (void)export_data;
  (void)export_data_len;
  (void)remote_addr;
  (void)remote_addr_len;

  return DOCA_SUCCESS;
}
  
doca_error_t DOCACopier::ImportFromFile(char *export_data, size_t *export_data_len,
    char **remote_addr, size_t *remote_addr_len, uint32_t memory_region_id) {
  
  FILE *fp;
  long file_size;
  char buffer[IMPORT_BUF_SIZE];
  std::string fname_exportdata;
  std::string fname_meminfo;

  fname_exportdata = std::string("/tmp/exportdata_") + std::to_string(memory_region_id)
                         + std::string(".bin");

  fname_meminfo = std::string("/tmp/meminfo_") + std::to_string(memory_region_id)
                         + std::string(".txt");

  fp = fopen(fname_exportdata.c_str(), "r");
  if (fp == NULL) {
    DOCA_LOG_ERR("Failed to open %s", fname_exportdata.c_str());
    return DOCA_ERROR_IO_FAILED;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    DOCA_LOG_ERR("Failed to calculate file size");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }

  file_size = ftell(fp);
  if (file_size == -1) {
    DOCA_LOG_ERR("Failed to calculate file size");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }

  if (file_size > MAX_EXPORT_SIZE)
    file_size = MAX_EXPORT_SIZE;

  *export_data_len = file_size;

  if (fseek(fp, 0L, SEEK_SET) != 0) {
    DOCA_LOG_ERR("Failed to calculate file size");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }

  if (fread(export_data, 1, file_size, fp) != (size_t)file_size) {
    DOCA_LOG_ERR("Failed to read export data");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }

  fclose(fp);

  /* Read source buffer information from file */
  fp = fopen(fname_meminfo.c_str(), "r");
  if (fp == NULL) {
    DOCA_LOG_ERR("Failed to open %s", fname_meminfo.c_str());
    return DOCA_ERROR_IO_FAILED;
  }

  /* Get source buffer address */
  if (fgets(buffer, IMPORT_BUF_SIZE, fp) == NULL) {
    DOCA_LOG_ERR("Failed to read the source (host) buffer address");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }
  *remote_addr = (char *)strtoull(buffer, NULL, 0);

  memset(buffer, 0, IMPORT_BUF_SIZE);

  /* Get source buffer length */
  if (fgets(buffer, IMPORT_BUF_SIZE, fp) == NULL) {
    DOCA_LOG_ERR("Failed to read the source (host) buffer length");
    fclose(fp);
    return DOCA_ERROR_IO_FAILED;
  }
  *remote_addr_len = strtoull(buffer, NULL, 0);

  fclose(fp);

  return DOCA_SUCCESS;
}

doca_error_t DOCACopier::ReceiveMemInfo(uint32_t memory_region_id) {
  char export_data[MAX_EXPORT_SIZE] = {0};
  size_t export_data_len = {0};
  doca_error_t result;
  
  /* Receive memory region information over socket */
  //result = ReceiveSocket(export_data, &export_data_len, &remote_addr, &remote_addr_len);

  /* Import memory region information from file */
  result = ImportFromFile(export_data, &export_data_len, &remote_addr,
              &remote_addr_len, memory_region_id);

  DOCA_LOG_INFO("Export data len: %lu", export_data_len);
  DOCA_LOG_INFO("VA: %lu", (uint64_t)remote_addr);
  DOCA_LOG_INFO("VA len: %lu", remote_addr_len);
  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("ImportFromFile failed");
    return DOCA_ERROR_NOT_CONNECTED;
  }

  /* Create remote mmap */
  result = doca_mmap_create_from_export((union doca_data *)NULL, (const void *)export_data,
                                  export_data_len, state.dev, &remote_mmap);

  if (result != DOCA_SUCCESS) {
    DOCA_LOG_ERR("doca_mmap_create_from_export failed");
    return result;
  }
  
  DOCA_LOG_INFO("Successfully exported memory");

  return DOCA_SUCCESS;
}
  
std::unique_ptr<Copier::CopyHandle> DOCACopier::StartDMA(void *dst, uint64_t dst_off,
    void *src, uint64_t src_off, uint64_t len, bool dst_is_pkt) {

  struct doca_buf *src_doca_buf;
  struct doca_buf *dst_doca_buf;
  struct doca_dma_task_memcpy *dma_task;
  doca_error_t result;
  uint16_t refcount;

  /* Find the local mmap that covers this packet buffer. */
  void *pkt_ptr = dst_is_pkt ? (char *)dst + dst_off : (char *)src + src_off;
  struct doca_mmap *local_mmap = nullptr;
  for (const auto &seg : local_segs_) {
    if (pkt_ptr >= seg.addr && pkt_ptr < (char *)seg.addr + seg.len) {
      local_mmap = seg.mmap;
      break;
    }
  }
  if (local_mmap == nullptr) {
    LOG(FATAL) << "Packet buffer " << pkt_ptr
               << " not covered by any registered DMA segment";
  }

  if (dst_is_pkt) {
    /*
     * DMA read: copy from host memory (remote_mmap/src) to packet buffer (local_mmap/dst)
     */
    result = doca_buf_inventory_buf_get_by_addr(state.buf_inv, local_mmap,
                (char *)dst + dst_off, len, &dst_doca_buf);
    if (result != DOCA_SUCCESS) {
      LOG(FATAL) << "Unable to get dst DOCA buf (read): " << doca_error_get_descr(result)
                 << " dst=" << (void *)((char *)dst + dst_off) << " len=" << len;
    }

    result = doca_buf_inventory_buf_get_by_addr(state.buf_inv, remote_mmap,
                remote_addr + src_off, len, &src_doca_buf);
    if (result != DOCA_SUCCESS) {
      doca_buf_dec_refcount(dst_doca_buf, &refcount);
      LOG(FATAL) << "Unable to get src DOCA buf (read): " << doca_error_get_descr(result)
                 << " remote_addr=" << (void *)(remote_addr + src_off) << " len=" << len;
    }

    result = doca_buf_set_data(src_doca_buf, remote_addr + src_off, len);
    if (result != DOCA_SUCCESS) {
      LOG(FATAL) << "Failed to set data for src buf (read): " << doca_error_get_descr(result);
    }
  } else {
    /*
     * DMA write: copy from packet buffer (local_mmap/src) to host memory (remote_mmap/dst)
     */
    result = doca_buf_inventory_buf_get_by_addr(state.buf_inv, local_mmap,
                (char *)src + src_off, len, &src_doca_buf);
    if (result != DOCA_SUCCESS) {
      LOG(FATAL) << "Unable to get src DOCA buf (write): " << doca_error_get_descr(result)
                 << " src=" << (void *)((char *)src + src_off) << " len=" << len;
    }

    result = doca_buf_inventory_buf_get_by_addr(state.buf_inv, remote_mmap,
                remote_addr + dst_off, len, &dst_doca_buf);
    if (result != DOCA_SUCCESS) {
      doca_buf_dec_refcount(src_doca_buf, &refcount);
      LOG(FATAL) << "Unable to get dst DOCA buf (write): " << doca_error_get_descr(result)
                 << " remote_addr=" << (void *)(remote_addr + dst_off) << " len=" << len;
    }

    result = doca_buf_set_data(src_doca_buf, (char *)src + src_off, len);
    if (result != DOCA_SUCCESS) {
      LOG(FATAL) << "Failed to set data for src buf (write): " << doca_error_get_descr(result);
    }
  }

  /* Allocate and initialize DMA task */
  TaskCompletionData *task_data = new TaskCompletionData{DOCA_SUCCESS, false};
  union doca_data user_data = {.ptr = task_data};

  result = doca_dma_task_memcpy_alloc_init(dma_ctx, src_doca_buf, dst_doca_buf,
                                            user_data, &dma_task);
  if (result != DOCA_SUCCESS) {
    doca_buf_dec_refcount(src_doca_buf, &refcount);
    doca_buf_dec_refcount(dst_doca_buf, &refcount);
    delete task_data;
    LOG(FATAL) << "Failed to allocate DMA task: " << doca_error_get_descr(result);
  }

  /* Submit DMA task */
  result = doca_task_submit(doca_dma_task_memcpy_as_task(dma_task));
  if (result != DOCA_SUCCESS) {
    doca_task_free(doca_dma_task_memcpy_as_task(dma_task));
    doca_buf_dec_refcount(src_doca_buf, &refcount);
    doca_buf_dec_refcount(dst_doca_buf, &refcount);
    delete task_data;
    LOG(FATAL) << "Failed to submit DMA task: " << doca_error_get_descr(result);
  }

  /* Release buffer references (task holds its own reference) */
  doca_buf_dec_refcount(src_doca_buf, &refcount);
  doca_buf_dec_refcount(dst_doca_buf, &refcount);

  return std::make_unique<CopyHandle>(this, task_data);
}

void DOCACopier::CopyHandle::await() {
  /* Poll progress engine until task callback fires */
  while (!task_data->completed) {
    doca_pe_progress(copier->state.pe);
  }

  doca_error_t result = task_data->result;
  delete task_data;
  task_data = nullptr;

  if (result != DOCA_SUCCESS) {
    LOG(FATAL) << "DMA task failed: " << doca_error_get_descr(result)
               << " -- check /tmp/exportdata_1.bin and /tmp/meminfo_1.txt are current (host re-run required)";
  }
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

std::unique_ptr<Copier::CopyHandle> DMA::StartDMA(UNUSED bess::Packet *pkt) {
  req_pkt_t *req_pkt = reinterpret_cast<req_pkt_t *>(pkt->data());
  dma_req_t *req_dma = reinterpret_cast<dma_req_t *>(&req_pkt->dma_req);
  
  size_t pkt_len = sizeof(struct ethhdr) + ntohs(req_pkt->ip.tot_len);

  uint64_t src_off = DMA_ADDR_OFFSET(req_dma->src_addr);
  uint8_t src_mem_id = DMA_ADDR_MEM_ID(req_dma->src_addr);
  uint64_t dst_off = DMA_ADDR_OFFSET(req_dma->dst_addr);
  uint8_t dst_mem_id = DMA_ADDR_MEM_ID(req_dma->dst_addr);
  uint64_t size = req_dma->size;

  if (NAAM_DEBUG)
    dump_dma_req(req_dma);
  
  // Set packet data as memory region 0
  memory_regions[MEM_ID_PKT] = reinterpret_cast<void *>(pkt->data());
  region_length[MEM_ID_PKT] = pkt->data_len();

// Set host bit if request is serverd from host
#if __x86_64__
  req_pkt->server_type = 1;
#endif

  /* read operation */
  if (req_dma->op == DMA_READ_OP) {
    /*
     * If destination is packet buffer then copy data from memory region 
     * Otherwise invalid request
     */
    if (DMA_ADDR_MEM_ID(req_dma->dst_addr) == 0) {
      if (dst_off >= offsetof(req_pkt_t, data) && (dst_off + size) < 1500) {
        
        // Needs to increase packet length first
        if (dst_off + size > pkt_len) {
          size_t new_len = dst_off + size;
          
          // Update IP header length
          req_pkt->ip.tot_len = htons(new_len - sizeof(struct ethhdr));
          
          // Update bess packet length
          uint16_t len = pkt->data_len();
          uint32_t total_len = pkt->total_len();
          pkt->set_data_len(len + (new_len - pkt_len));
          pkt->set_total_len(total_len + (new_len - pkt_len));
        }
      
        // Record stats
        if (mem_type == MemType::LDMA)
          num_bytes_dma += size;
        
        if (src_off + size > region_length[src_mem_id]) {
          LOG(FATAL) << "DMA_READ: src out of bounds:"
                     << " mem_id=" << (int)src_mem_id
                     << " src_off=" << src_off
                     << " size=" << size
                     << " region_len=" << region_length[src_mem_id]
                     << " — BPF computed invalid offset (hashtable full or corrupted)";
        }

        // Update status to success
        req_dma->status = RET_DMA_SUCCESS;

        return copier->StartDMA(pkt->data(), dst_off,
                                memory_regions[src_mem_id], src_off,
                                size, true);
      }
      else {
        // Update status to failure
        req_dma->status = RET_DMA_FAILURE;
        printf("DMA_READ: Invalid packet buffer region for DMA <Private filed || dma_region || MTU>\n");
        return nullptr;
      }
    }
    else {
      LOG(ERROR) << "DMA_READ: memory ID should be 0. Got " << DMA_ADDR_MEM_ID(req_dma->dst_addr) << "\n";
      return nullptr;
    }
  } 
  
  // Size field is used to pack old and new values for CAS as argument
  // Return value is also packed in the size field
  else if (req_dma->op == DMA_CAS_OP) {
    if (dst_off + sizeof(CAS_TYPE) > region_length[dst_mem_id]) {
      LOG(FATAL) << "DMA_CAS: dst out of bounds:"
                 << " mem_id=" << (int)dst_mem_id
                 << " dst_off=" << dst_off
                 << " region_len=" << region_length[dst_mem_id]
                 << " — BPF computed invalid offset (hashtable full or corrupted)";
    }
    CAS_TYPE ret = __sync_val_compare_and_swap(
                          (CAS_TYPE *)((char *)memory_regions[dst_mem_id] + dst_off),
                          (CAS_TYPE)CAS_UNPACK_OLDVAL(size),
                          (CAS_TYPE)CAS_UNPACK_NEWVAL(size));

    // Encode CAS return value to lower 32 bits of size field
    // of dma descriptor in packet buffer
    req_dma->size = CAS_PACK(0, ret);
    
    return std::make_unique<MemCopier::CopyHandle>();
  }

  // size field is used to return the value of the atomic operation
  else if (req_dma->op == DMA_FAA_OP) {
    if (dst_off + sizeof(uint64_t) > region_length[dst_mem_id]) {
      LOG(FATAL) << "DMA_FAA: dst out of bounds:"
                 << " mem_id=" << (int)dst_mem_id
                 << " dst_off=" << dst_off
                 << " region_len=" << region_length[dst_mem_id]
                 << " — BPF computed invalid offset (hashtable full or corrupted)";
    }
    uint64_t ret = __atomic_fetch_add((uint64_t *)((char *)memory_regions[dst_mem_id] + dst_off),
                                        size, __ATOMIC_SEQ_CST);
                          

    // Push FAA return value to the size field
    // of dma descriptor in packet buffer
    req_dma->size = ret;

    return std::make_unique<MemCopier::CopyHandle>();
  }
  
  /* write operation */
  else if (req_dma->op == DMA_WRITE_OP) {
    if (src_off + size > region_length[src_mem_id]) {
      LOG(FATAL) << "DMA_WRITE: src out of bounds:"
                 << " mem_id=" << (int)src_mem_id
                 << " src_off=" << src_off
                 << " size=" << size
                 << " region_len=" << region_length[src_mem_id]
                 << " — BPF computed invalid offset (hashtable full or corrupted)";
    }
    if (dst_off + size > region_length[dst_mem_id]) {
      LOG(FATAL) << "DMA_WRITE: dst out of bounds:"
                 << " mem_id=" << (int)dst_mem_id
                 << " dst_off=" << dst_off
                 << " size=" << size
                 << " region_len=" << region_length[dst_mem_id]
                 << " — BPF computed invalid offset (hashtable full or corrupted)";
    }

    // Record stats
    if (mem_type == MemType::LDMA)
      num_bytes_dma += size;

    // Update status to success
    req_dma->status = RET_DMA_SUCCESS;

    return copier->StartDMA((char *)memory_regions[dst_mem_id], dst_off,
                    (char *)memory_regions[src_mem_id], src_off, size, false);

  } 

  LOG(ERROR) << "StartDMA:" << " Operation " << (uint64_t)req_dma->op << " is invalid\n";
  return nullptr;
}


void DMA::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  assert(batch->cnt() <= MAX_BATCH_SIZE
      && LOG(ERROR) << "Maximum supported batch size: " << MAX_BATCH_SIZE);

  for (uint8_t i = 0; i < batch->cnt(); i++) {
    queue.emplace_back(StartDMA(batch->pkts()[i]));
  }
  
  for (auto &handle : queue) {
    if (handle) {
      handle->await();
    }
  }
  
  // clean up
  queue.clear();

  RunNextModule(ctx, batch);
}

ADD_MODULE(DMA, "DMA", "emulating dma module in BESS")
