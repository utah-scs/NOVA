#ifndef BESS_MODULES_DMA_H_
#define BESS_MODULES_DMA_H_

// Headers for this file
#include "module.h"
#include "utils/endian.h"
#include "../utils/dma_common.h"
#include <rte_memory.h>

extern "C" {
  #include "naam.h"
}

#include "pb/dma_msg.pb.h"

// Standard headers
#include <unordered_map>
#include <vector>

// DOCA header
#include <doca_error.h>

#define MAX_MEM_REG 10
#define MEM_ID_PKT 0
#define MAX_BATCH_SIZE 32 
#define MAX_MEM_SEG 1024
#define MAX_DOCA_BUF (MAX_BATCH_SIZE * 2)  /* 2 bufs per task × max batch */
#define MAX_EXPORT_SIZE 1024
#define IMPORT_BUF_SIZE 256

class Copier {
public:
  class CopyHandle {
    public:
      virtual void await(void) = 0;
  };
  
  Copier() {};
  virtual ~Copier() {};

  virtual std::unique_ptr<CopyHandle> StartDMA(void *dst, uint64_t dst_off,
                                               void *src, uint64_t src_off,
                                               uint64_t len, bool dst_is_pkt) = 0;
  
  virtual doca_error_t SendMemInfo(void *memory_region, size_t memory_region_len,
                                   uint32_t memory_region_id) = 0;

  virtual doca_error_t ReceiveMemInfo(uint32_t memory_region_id) = 0;
};

class MemCopier: public Copier {
public:
  class CopyHandle: public Copier::CopyHandle {
    public:
      virtual void await(void) override;
  };
  
  MemCopier(const char *pci_addr, const char *ip, uint16_t port):
    pci_addr(pci_addr), ip(ip), port(port)  {

#if __x86_64__
      if (pci_addr) {
        if (Init() != DOCA_SUCCESS) {
          LOG(ERROR) << "Initializing MemCopier failed!";
          abort();
        }
      }
#endif

    };

  ~MemCopier() {
    DeInit();
  }

  /* Send memory region information to DPU for DMA */
  virtual doca_error_t SendMemInfo(void *memory_region, size_t memory_region_len,
                                   uint32_t memory_region_id);

  /* Not implemented for MemCopier */
  virtual doca_error_t ReceiveMemInfo(uint32_t memory_region_id);
  
  /* Initiate DMA operation */
  virtual std::unique_ptr<Copier::CopyHandle> StartDMA(void *dst, uint64_t dst_off,
                                                       void *src, uint64_t src_off,
                                                       uint64_t len, bool dst_is_pkt);

private:
  /* open device and initialize necessary structures */
  doca_error_t Init();

  /* close device and destruct structures */
  void DeInit();

  /* Create socket and send memory region information to DPU */
  doca_error_t SendSocket(const char *export_data, size_t export_data_len,
                          uint64_t memory_region_addr, size_t memory_region_len);

  /*
   * Create export files for the memory regions
   * that can be imported in the DPU to register
   * the memory regions on the DPU.
   */

  doca_error_t ExportToFile(const char *export_data, size_t export_data_len,
                            uint64_t memory_region_addr, size_t memory_region_len,
                            uint32_t memory_region_id);

private:
  const char *pci_addr;
  const char *ip;
  uint16_t port;
  struct dma_state state;
};


class DOCACopier: public Copier {
public:
  struct TaskCompletionData {
    doca_error_t result;
    bool completed;
  };

  struct MemSeg {
    void *addr;
    size_t len;
    struct doca_mmap *mmap;
  };

  class CopyHandle: public Copier::CopyHandle {
    public:
      CopyHandle(DOCACopier *copier, TaskCompletionData *data)
        : copier(copier), task_data(data) {}

      virtual void await(void) override;

    private:
      DOCACopier *copier;
      TaskCompletionData *task_data;
  };
  
  DOCACopier(const char *pci_addr);

  ~DOCACopier() {
    DeInit();
  }
  
  /* Not implemented for DOCACopier */
  virtual doca_error_t SendMemInfo(void *memory_region, size_t memory_region_len,
                                   uint32_t memory_region_id);
  
  /* Receive memory region inforamtion from host for DMA */
  virtual doca_error_t ReceiveMemInfo(uint32_t memory_region_id);
  
  /* Initiate DMA operation */
  virtual std::unique_ptr<Copier::CopyHandle> StartDMA(void *dst, uint64_t dst_off,
                                                       void *src, uint64_t src_off,
                                                       uint64_t len, bool dst_is_pkt);

private:
  /* Open device and initialize necessary structures */
  doca_error_t Init();

  /* Close device and destruct structures */
  void DeInit();

  /* Receive meminfo from host on a socket */
  doca_error_t ReceiveSocket(char *export_data, size_t *export_data_len,
                             char **remote_addr, size_t *remote_addr_len);
  
  /* Import memory information from exported files */
  doca_error_t ImportFromFile(char *export_data, size_t *export_data_len,
                              char **remote_addr, size_t *remote_addr_len,
                              uint32_t memory_region_id);

private:
  const char *pci_addr;
  uint16_t port;
  
  struct dma_state state;
  struct doca_dma *dma_ctx = nullptr;
  struct doca_mmap *remote_mmap = nullptr;
  char *remote_addr = NULL;
  size_t remote_addr_len = {0};
  std::vector<MemSeg> local_segs_;
};


class DMA final : public Module {
public:
  static const Commands cmds;
  std::unique_ptr<Copier> copier;

  DMA():
    Module(),
    queue()
    {
      queue.reserve(MAX_BATCH_SIZE);
    }

  // DMA = LDMA to differentiate from class name
  enum MemType {
    LOCAL = 0,
    LDMA,
    RDMA
  };
  
  CommandResponse Init(const dma::dmatrans::pb::DMAArg &arg);
  void DeInit() override;
  
  CommandResponse AddMemory(const dma::dmatrans::pb::DMAArg &arg);
  CommandResponse ClearMemory(const bess::pb::EmptyArg &arg);
  CommandResponse GetDMAStats(const bess::pb::EmptyArg &arg);

  int RegisterMemory(uint32_t region_id, uint64_t region_size, std::string ds, bool initialize);
  int SetupLinkedList(uint32_t region_id, uint64_t size);
  int SetupHashTable(uint32_t region_id);
  int SetupBPTree(uint32_t region_id);
  uint64_t GetRandomNumber(struct rand_state *state);

  std::unique_ptr<Copier::CopyHandle> StartDMA(bess::Packet *pkt);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

private:
  void insertSHMInfo(int id, std::string &name, int fd, int size) {
    shmInfo.emplace(id, std::make_tuple(name, fd, size)); 
  }

private:
  // container for keeping information about opened shared memory file
  // attached to each memory region id
  std::unordered_map<uint32_t, std::tuple<std::string, int, int>> shmInfo;
  
  // List of mapped shared memory
  void *memory_regions[MAX_MEM_REG];
  uint64_t region_length[MAX_MEM_REG];
  
  // Queue of in flight DMA operations
  std::vector<std::unique_ptr<Copier::CopyHandle>> queue;

  // Memory type
  MemType mem_type;
  
  // Enable/disable support for DOCA DMA
  bool doca_dma;
  
  // pcie address to use for DOCA DMA
  const char *pci_addr;

  // Lock for compare and swap
  std::mutex cas_lock;

  // Number of bytes moved through DOCA DMA
  uint64_t num_bytes_dma;

  // Hashtable sizing (set from proto arg or compiled-in defaults)
  uint64_t num_keys;
  uint32_t num_buckets;
};

#endif // BESS_MODULES_DMA_H_
