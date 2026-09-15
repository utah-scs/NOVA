#include <stdint.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

static int ebpf_rdma_read(int a) {
	return a;
}

uint64_t prog(void *pkt) {



     // int a = 0x8;
     // int b = ebpf_rdma_read(a);

     // b = b + 1;
     // b = ebpf_rdma_read(b);

     // return b;

     // int a = 12;

     // a  = a + 1;
     // int b  = a;
     // b = b / 2;

     // int c = 18;
     // int v = 32 + c;
     // int h = 12 + v ;
     // int i = 81 + h;
     // int g = 12 + a + i;
	     
     
     if(!pkt)
	     return 10;
    struct ether_header *ether_header = pkt;
    if (ether_header->ether_type != __builtin_bswap16(0x0800))
         return 1;

    struct iphdr *iphdr = (void *)(ether_header + 1);
    // if (iphdr->protocol != 17 || (iphdr->frag_off & 0x1ffff) != 0 ||
    //   iphdr->daddr == __builtin_bswap32(0x1020304)) {
    if (iphdr->protocol == 17) {
      return 2;
    }
    //  
    //  int hlen = iphdr->ihl * 4;
    //  // struct udphdr *udphdr = (void *)iphdr + hlen;
    //  struct udphdr *udphdr = (void *) (iphdr + 1);
    //  if (udphdr->dest == __builtin_bswap16(319))
    //      return 3;
    return 0;
    // return 4;
}
