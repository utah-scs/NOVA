/*
 * Copyright 2015 Big Switch Networks, Inc
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

#ifndef BESS_UTILS_UBPF_UTIL_H
#define BESS_UTILS_UBPF_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// RET_DMA_HELPER is defined in naam.h
// Ideally this file should include headers
// however it's generating compiler error
// So it's included in ubpf_util.cc instead
// and RET_DMA_HELPER is redefined as ubpf_util.h
// is included in modules/ubpf.cc and it uses this.
#define RET_DMA_HELPER 0xffffffffffffffff

struct ubpf_vm;
struct xdp_md;
typedef uint64_t (*ubpf_jit_fn)(void *mem, size_t mem_len);

namespace bess {
namespace utils {
  int run_bpf_code(struct ubpf_vm *vm, uint64_t (*fn)(void *, size_t), struct xdp_md *ctx, size_t pkt_len, uint64_t *ret_val);
  size_t load_bpf_code(const char *file_path, char **code_address);
  struct ubpf_vm *init_engine(const char *code, size_t code_len);
  int destroy_engine(struct ubpf_vm *vm);
  int vm_size(void);
  int dma_req_size(void);
  ubpf_jit_fn jit_compile(struct ubpf_vm *vm);
} // namespace utils
} // namespace bess

#endif
