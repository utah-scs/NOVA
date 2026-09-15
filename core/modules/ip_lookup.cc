// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.  //
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "ip_lookup.h"

#include <rte_config.h>
#include <rte_errno.h>
#include <rte_lpm.h>
#include <rte_jhash.h>

#include "../utils/bits.h"
#include "../utils/ether.h"
#include "../utils/format.h"
#include "../utils/ip.h"

#define VECTOR_OPTIMIZATION 0

// static inline int found_in_hashmap(int key, std::unordered_map<int, int> umap) {
//   if (umap.find(key) == umap.end()) 
//     return -1;
//   else
//     // cout << "Found " << key << "\n\n"; 
//     return umap[key];
// }


static inline int is_valid_gate(gate_idx_t gate) {
  return (gate < MAX_GATES || gate == DROP_GATE);
}

const Commands IPLookup::cmds = {
    {"add", "IPLookupCommandAddArg", MODULE_CMD_FUNC(&IPLookup::CommandAdd),
     Command::THREAD_UNSAFE},
    {"delete", "IPLookupCommandDeleteArg", MODULE_CMD_FUNC(&IPLookup::CommandDelete),
     Command::THREAD_UNSAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&IPLookup::CommandClear),
     Command::THREAD_UNSAFE},
    {"load", "IPLookupCommandLoadArg", MODULE_CMD_FUNC(&IPLookup::CommandLoad), Command::THREAD_UNSAFE}};

CommandResponse IPLookup::Init(const bess::pb::IPLookupArg &arg) {
  // struct rte_hash_parameters ut_params = {
  // 	.entries = 1024,
  // 	.key_len = 32, /* 13 */
  // 	.hash_func = rte_jhash,
  // 	.hash_func_init_val = 0,
  // 	.socket_id = 0,
  //       // .name = "hashmap",
  // };

  struct rte_hash_parameters ut_params;//  = {
  ut_params.entries = 4096;
  ut_params.key_len = 4;
  ut_params.hash_func = rte_jhash;
  ut_params.hash_func_init_val = 0;
  ut_params.socket_id = 0;
  ut_params.name = "hashmap";

  struct rte_lpm_config conf = {
      .max_rules = arg.max_rules() ? arg.max_rules() : 4096,
      /// .number_tbl8s = arg.max_tbl8s() ? arg.max_tbl8s() : 128,
      .number_tbl8s = arg.max_tbl8s() ? arg.max_tbl8s() : 1024,
      .flags = 0,
  };

  default_gate_ = DROP_GATE;

  lpm_ = rte_lpm_create(name().c_str(), /* socket_id = */ 0, &conf);

  if (!lpm_) {
    return CommandFailure(rte_errno, "DPDK error: %s", rte_strerror(rte_errno));
  }

  hashmap_ = rte_hash_create(&ut_params);

  if (!hashmap_) {
    return CommandFailure(rte_errno, "DPDK error: %s", rte_strerror(rte_errno));
  }

  return CommandSuccess();
}

// void IPLookup::LoadMap(std::string ip, const int gate) {
//   be32_t addr;
//   bess::utils::ParseIpv4Address(ip, &addr);
//   umap_.insert(std::make_pair(addr.value(), gate));
//   return;
// }

void IPLookup::DeInit() {
  if (lpm_) {
    rte_lpm_free(lpm_);
  }

  if (hashmap_) {
    rte_hash_free(hashmap_);
  }
}

void IPLookup::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::ToIpv4Address;

  gate_idx_t default_gate = default_gate_;

  int cnt = batch->cnt();
  int i = 0;
  // static void ** val = NULL;
  // if(!val) val = (void **) malloc(1 * sizeof(void *));

  // int hash_gate = -1;

#if VECTOR_OPTIMIZATION
  // Convert endianness for four addresses at the same time
  const __m128i bswap_mask =
      _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);

  /* 4 at a time */
  for (i = 0; i + 3 < cnt; i += 4) {
    Ethernet *eth;
    Ipv4 *ip;

    uint32_t a0, a1, a2, a3;
    uint32_t next_hops[4];

    __m128i ip_addr;

    eth = batch->pkts()[i]->head_data<Ethernet *>();
    ip = (Ipv4 *)(eth + 1);
    a0 = ip->dst.raw_value();

    eth = batch->pkts()[i + 1]->head_data<Ethernet *>();
    ip = (Ipv4 *)(eth + 1);
    a1 = ip->dst.raw_value();

    eth = batch->pkts()[i + 2]->head_data<Ethernet *>();
    ip = (Ipv4 *)(eth + 1);
    a2 = ip->dst.raw_value();

    eth = batch->pkts()[i + 3]->head_data<Ethernet *>();
    ip = (Ipv4 *)(eth + 1);
    a3 = ip->dst.raw_value();

    ip_addr = _mm_set_epi32(a3, a2, a1, a0);
    ip_addr = _mm_shuffle_epi8(ip_addr, bswap_mask);

    rte_lpm_lookupx4(lpm_, ip_addr, next_hops, default_gate);

    EmitPacket(ctx, batch->pkts()[i], next_hops[0]);
    EmitPacket(ctx, batch->pkts()[i + 1], next_hops[1]);
    EmitPacket(ctx, batch->pkts()[i + 2], next_hops[2]);
    EmitPacket(ctx, batch->pkts()[i + 3], next_hops[3]);
  }
#endif

  /* process the rest one by one */
  for (; i < cnt; i++) {
    Ethernet *eth;
    Ipv4 *ip;

    uint32_t next_hop;
    int ret;
    uint32_t key;

    eth = batch->pkts()[i]->head_data<Ethernet *>();
    ip = (Ipv4 *)(eth + 1);

    key = ip->dst.value();

    if (key == 0x0a000000 || key == 0x0a030000 || key == 0x0a140000 || key == 0x0a270000) {
      EmitPacket(ctx, batch->pkts()[i], 1);
      continue;
    }

    ret = rte_lpm_lookup(lpm_, ip->dst.value(), &next_hop);

    if (ret == 0) {
      EmitPacket(ctx, batch->pkts()[i], next_hop);
    } else {
      EmitPacket(ctx, batch->pkts()[i], default_gate);
    }

    // key = ip->dst.value();
    // ret = rte_hash_lookup(hashmap_, reinterpret_cast<const void *>(&key));
    //
    // if (ret < 0) {
    //   EmitPacket(ctx, batch->pkts()[i], default_gate);
    //   LOG(INFO) << ret << " do we have a miss? " << bess::utils::ToIpv4Address(ip->dst) << " " << key << " \n";
    // } else {
    //   // LOG(INFO) << ret << " Hit " << bess::utils::ToIpv4Address(ip->dst) << " " << key << " \n";
    //   next_hop = 4;
    //   EmitPacket(ctx, batch->pkts()[i], next_hop);
    // }
  }
}

ParsedPrefix IPLookup::ParseIpv4Prefix(
    const std::string &prefix, uint64_t prefix_len) {
  using bess::utils::Format;
  be32_t net_addr;
  be32_t net_mask;

  if (!prefix.length()) {
    return std::make_tuple(EINVAL, "prefix' is missing", be32_t(0));
  }
  if (!bess::utils::ParseIpv4Address(prefix, &net_addr)) {
    return std::make_tuple(EINVAL,
			   Format("Invalid IP prefix: %s", prefix.c_str()),
			   be32_t(0));
  }

  if (prefix_len > 32) {
    return std::make_tuple(EINVAL,
			   Format("Invalid prefix length: %" PRIu64,
				  prefix_len),
			   be32_t(0));
  }

  net_mask = be32_t(bess::utils::SetBitsLow<uint32_t>(prefix_len));
  if ((net_addr & ~net_mask).value()) {
    return std::make_tuple(EINVAL,
			   Format("Invalid IP prefix %s/%" PRIu64 " %x %x",
				  prefix.c_str(), prefix_len, net_addr.value(),
				  net_mask.value()),
			   be32_t(0));
  }
  return std::make_tuple(0, "", net_addr);
}

CommandResponse IPLookup::CommandLoad(
    const bess::pb::IPLookupCommandLoadArg &arg) {
  using bess::utils::ToIpv4Address;
  be32_t addr;
  int ret;

  uint32_t key;
  uint8_t value;
  // uint32_t val = 0;// = (int *) malloc(1 * sizeof(int *));

  bess::utils::ParseIpv4Address(arg.ip(), &addr);

  key = addr.value();
  value = (uint8_t) arg.gate();

  LOG(INFO) << "Loaded IP: " << bess::utils::ToIpv4Address(addr) << " " << key << "\n";

  // umap_.insert(std::make_pair(addr.value(), arg.gate()));
  // umap_.Insert(key, value);

  // DPDK hashtable
  ret = rte_hash_add_key_data(hashmap_, reinterpret_cast<const void *>(&key), reinterpret_cast<void *>(&value));
  if (ret != 0)
    return CommandFailure(-ret, "DPDK error: %s", rte_strerror(ret));

  ret = rte_hash_lookup(hashmap_, reinterpret_cast<const void *>(&key));
  if (ret < 0)
     return CommandFailure(ret, "shit is going on");
  LOG(INFO) << ret << "\n";

  return CommandSuccess();
}

CommandResponse IPLookup::CommandAdd(
    const bess::pb::IPLookupCommandAddArg &arg) {
  gate_idx_t gate = arg.gate();
  uint64_t prefix_len = arg.prefix_len();
  ParsedPrefix prefix = ParseIpv4Prefix(arg.prefix(), prefix_len);
  if (std::get<0>(prefix)) {
    return CommandFailure(std::get<0>(prefix), "%s",
			  std::get<1>(prefix).c_str());
  }

  if (!is_valid_gate(gate)) {
    return CommandFailure(EINVAL, "Invalid gate: %hu", gate);
  }

  if (prefix_len == 0) {
    default_gate_ = gate;
  } else {
    be32_t net_addr = std::get<2>(prefix);
    int ret = rte_lpm_add(lpm_, net_addr.value(), prefix_len, gate);
    if (ret) {
      return CommandFailure(-ret, "rpm_lpm_add() failed");
    }
  }

  return CommandSuccess();
}

CommandResponse IPLookup::CommandDelete(
    const bess::pb::IPLookupCommandDeleteArg &arg) {
  uint64_t prefix_len = arg.prefix_len();
  ParsedPrefix prefix = ParseIpv4Prefix(arg.prefix(), prefix_len);
  if (std::get<0>(prefix)) {
    return CommandFailure(std::get<0>(prefix), "%s",
			  std::get<1>(prefix).c_str());
  }

  if (prefix_len == 0) {
    default_gate_ = DROP_GATE;
  } else {
    be32_t net_addr = std::get<2>(prefix);
    int ret = rte_lpm_delete(lpm_, net_addr.value(), prefix_len);
    if (ret) {
      return CommandFailure(-ret, "rpm_lpm_delete() failed");
    }
  }

  return CommandSuccess();
}

CommandResponse IPLookup::CommandClear(const bess::pb::EmptyArg &) {
  rte_lpm_delete_all(lpm_);
  return CommandSuccess();
}

ADD_MODULE(IPLookup, "ip_lookup",
           "performs Longest Prefix Match on IPv4 packets")
