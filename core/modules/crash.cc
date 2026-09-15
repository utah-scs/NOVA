// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
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

#include "crash.h"

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/tcp.h"
#include "../utils/udp.h"

#include "../utils/endian.h"

void Crash::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Tcp;
  using bess::utils::Udp;
  using bess::utils::be16_t;
  using bess::utils::be32_t;

  int cnt = batch->cnt();
  void *data;

  for (int i = 0; i < cnt; i++) {
    data = reinterpret_cast<void *>(batch->pkts()[i]->data());
    req_pkt_t *req_pkt = reinterpret_cast<req_pkt_t *>(data);
    
    // If the first byte of the data region is 1, crash the module
    if (*(uint8_t *)req_pkt->data) {
      // crash the module
      int *ptr = nullptr;
      *ptr = 0;
    }
    
    Ethernet *eth = batch->pkts()[i]->head_data<Ethernet *>();
    Ethernet::Address tmp;

    tmp = eth->dst_addr;
    eth->dst_addr = eth->src_addr;
    eth->src_addr = tmp;

    // swap IP addresses if the packet is IPv4
    if (eth->ether_type == be16_t(Ethernet::Type::kIpv4)) {
      struct Ipv4 *ip = reinterpret_cast<struct Ipv4 *>(eth + 1);
      be32_t tmp = ip->src;;
      ip->src = ip->dst;
      ip->dst = tmp;
    }

    // swap tcp/udp ports if the packet is IPv4
    if (eth->ether_type == be16_t(Ethernet::Type::kIpv4)) {
      struct Ipv4 *ip = reinterpret_cast<struct Ipv4 *>(eth + 1);
      if (ip->protocol == Ipv4::Proto::kTcp) {
        struct Tcp *tcp = reinterpret_cast<struct Tcp *>(ip + 1);
        be16_t tmp = tcp->src_port;
        tcp->src_port = tcp->dst_port;
        tcp->dst_port = tmp;
      } else if (ip->protocol == Ipv4::Proto::kUdp) {
        struct Udp *udp = reinterpret_cast<struct Udp *>(ip + 1);
        be16_t tmp = udp->src_port;
        udp->src_port = udp->dst_port;
        udp->dst_port = tmp;
      }
    }
    
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(Crash, "crash", "A module that crashes bess if faulty packets are received")
