/* x86_64 BPF JIT code was adopted from FreeBSD 10 - Sangjin */

/*-
 * Copyright (C) 2002-2003 NetGroup, Politecnico di Torino (Italy)
 * Copyright (C) 2005-2009 Jung-uk Kim <jkim@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Politecnico di Torino nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <bit>
#include <bitset>
#include <fstream>

#include "ubpf.h"
#include "../utils/ubpf_util.h"

/* -------------------------------------------------------------------------
 * Module code begins from here
 * ------------------------------------------------------------------------- */

const Commands UBPF::cmds = {};

CommandResponse UBPF::Init(const bess::pb::UBPFArg &arg) {
  if (arg.bpf_enable_jit()) {
    LOG(INFO) << "JIT compilation enabled\n";
  }
  else {
    LOG(INFO) << "JIT compilation disabled\n";
  }

  num_funcs = arg.functions().size();
  if (num_funcs > MAX_FUNCS) {
    return CommandFailure(EINVAL, "Too many functions");
  }

  if (num_funcs == 0) {
    return CommandFailure(EINVAL, "No functions provided");
  }

  for (const auto &func : arg.functions()) {
    auto ret = LoadElf(func, arg.bpf_enable_jit());
    
    if (ret)
      return CommandFailure(EINVAL, "Failed load eBPF ELF file");
  }
    
  return CommandSuccess();
}

void UBPF::DeInit() {
  for (int i = 0; i < MAX_FUNCS; i++) {
    if (vm[i])
      bess::utils::destroy_engine(vm[i]);
  }
}

int UBPF::LoadElf(const bess::pb::UBPFArg_Function &func, bool is_jit) {
  size_t code_length;
  char *code_address = NULL;
  int func_id = func.id();

  assert(func_id < 256 && "Function ID must be less than 256");
  
  if (!func.bpf_file_path().empty()) {
    code_length = bess::utils::load_bpf_code(func.bpf_file_path().c_str(), &code_address);
    
    LOG(INFO) << "File to load " << func.bpf_file_path() << "\n";

    if (code_address == NULL) {
      char cwd[256];
      if (!getcwd(cwd, 256))
        cwd[0] = '\0';

      LOG(ERROR) << "Failed to load " << func.bpf_file_path()
                 << " (cwd=" << cwd << ")\n";
      
      return 1;
    }
  }
  else {
    if (func.bpf_elf_bytes().empty()) {
        
      LOG(ERROR) << "No BPF file or bytecode provided\n";
                   
      return 1;
    }

    code_address = const_cast<char *>(func.bpf_elf_bytes().c_str());
    code_length = func.bpf_elf_bytes().length();
  }

  vm[func_id] = bess::utils::init_engine(code_address, code_length);
  
  if (!vm[func_id]) {
    LOG(ERROR) << "Failed to create the BPF virtual machine" << "\n";
    return 1;
  }

  if (is_jit == true) {
    char *errmsg;
    jit_funcs[func_id] = ubpf_compile(vm[func_id], &errmsg);
    if (jit_funcs[func_id] == NULL) {
      fprintf(stderr, "Failed to compile: %s\n", errmsg);
      free(errmsg);
      return 1;
    }
  }
  
  return 0;
}

void UBPF::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  void *data;
  size_t data_len;
  int ret;
  uint64_t bpf_ret_val;
  bess::Packet *pkt;

  uint32_t cnt = batch->cnt();

  for (uint32_t i = 0; i < cnt; i++) {
    pkt = batch->pkts()[i];
    data = reinterpret_cast<void *>(pkt->data());
    data_len = pkt->data_len();
    req_pkt_t *req_pkt = reinterpret_cast<req_pkt_t *>(data);
    int func_id = req_pkt->func_id;

    xdp_ctx.data = reinterpret_cast<long>(data);
    xdp_ctx.data_end = reinterpret_cast<long>((char *)data + data_len);

    if (func_id >= num_funcs) {
      LOG(ERROR) << "Invalid function ID: " << func_id << "\n";
      continue;
    }

    ret = bess::utils::run_bpf_code(vm[func_id], jit_funcs[func_id], &xdp_ctx, data_len, &bpf_ret_val);

    if (ret != 0)
      LOG(ERROR) << "Failed to run BPF program\n";

    /*
     * 1 from ebpf execution engine means
     * a preempted packet forward to DMA
     * through output gate 0
     *
     * 0 from ebpf execution engine means
     * end of ebpf program execution forward
     * to output gate 1
     */
    if (bpf_ret_val == RET_DMA_HELPER)
      EmitPacket(ctx, pkt, 0);
    else
      EmitPacket(ctx, pkt, 1);

  }
  
  // RunNextModule(ctx, batch);
}

ADD_MODULE(UBPF, "ubpf", "userspace eBPF engine")
