#include <generic/rte_cycles.h>
#include <errno.h>
#include <inttypes.h>
#include <rte_byteorder.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <unistd.h>
#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>
#include <rte_atomic.h>
#include <rte_flow.h>

#include "packet.h"
#include "zipf.h"
#include <math.h>

#define DEBUG_ARGS 1

#define MAX_TX_RX_QUEUE 16
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024
#define MAX_PKT_BURST 128
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

#define BURST_SIZE 32
#define RECV_BURST_SIZE 64
#define MAX_CORES 64
#define UDP_MAX_PAYLOAD 1472
#define MAX_SAMPLES ((uint64_t)1000000000)
#define RANDOM_US 10

#define SRC_IP_START MAKE_IP_ADDR(192, 168, 1, 10)
#define SRC_IP_MAX MAKE_IP_ADDR(192, 168, 1, 210)
#define SRC_PORT_START 1234
#define SRC_PORT_MAX 1243
#define DST_PORT_START 10000
#define DST_PORT_MAX 11000

#define PKT_SEQ_START 1

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.offloads = RTE_ETH_RX_OFFLOAD_IPV4_CKSUM,
		.mq_mode = RTE_ETH_MQ_RX_RSS,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_NONFRAG_IPV4_UDP | RTE_ETH_RSS_NONFRAG_IPV6_UDP,
		},
	},
	.txmode = {
		.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE,
	},
};

uint32_t kMagic = 0x6e626368; // 'nbch'

struct nbench_req {
  uint32_t magic;
  int nports;
};

struct nbench_resp {
  uint32_t magic;
  int nports;
  uint16_t ports[];
};

struct open_loop_arg {
	struct rte_ether_addr *eth_addr;
	uint8_t queue_id;
};

struct port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t unique_rx;   /* responses matched to a pending slot (excl. late duplicates) */
	uint64_t host_rx;     /* unique_rx responses served by host CPU (server_type == 1) */
	uint64_t dpu_rx;      /* unique_rx responses served by DPU     (server_type == 0) */
	uint64_t dropped;
	uint64_t wrong_queue; /* response dst_port not in this queue's port range */
} __rte_cache_aligned;

struct port_statistics port_statistics[MAX_TX_RX_QUEUE];

enum {
	PAYLOAD_TYPE_GET,
	PAYLOAD_TYPE_SET,
	PAYLOAD_TYPE_LIST,
	PAYLOAD_TYPE_HT,
	PAYLOAD_TYPE_BPT,
};

enum {
	LOADGEN_TYPE_FIXED = 0,
	LOADGEN_TYPE_VARIABLE,
	LOADGEN_TYPE_LADDER,
};

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

static unsigned int dpdk_port = 3;
static unsigned int send_batch_size = 1;
static unsigned int recv_batch_size = BURST_SIZE;
static uint8_t num_funcs = 1;
static uint8_t payload_type;
static uint8_t loadgen_type;
struct rte_mempool *rx_mbuf_pool;
struct rte_mempool *tx_mbuf_pool;
static struct rte_ether_addr my_eth;
static struct rte_ether_addr server_eth;
static uint32_t server_ip;
static int seconds;
static size_t payload_len;
static unsigned int server_port;
static unsigned int num_queues;
struct rte_ether_addr zero_mac = {
		.addr_bytes = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}
};
struct rte_ether_addr broadcast_mac = {
		.addr_bytes = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}
};

static uint64_t *diff_times;
char *output_filename = NULL;

static int warmup_seconds = 2;
static uint64_t warmup_end_cycles;

static uint64_t send_start_time;
static uint64_t send_end_time;
static uint64_t recv_start_time;
static uint64_t recv_end_time;
static long send_pps_start = 0;
static long send_pps_end = 0;
static long send_pps_step = 0;
static int send_pps_trigger_time = 0;

/*rte_atomic64_t num_get_req;*/
/*rte_atomic64_t num_set_req;*/

static struct get_next_ip {
	uint32_t ip;
} __rte_cache_aligned get_next_ip[MAX_TX_RX_QUEUE];

static struct get_next_port {
	uint16_t port;
} __rte_cache_aligned src_port_next[MAX_TX_RX_QUEUE];

static struct arr_index {
	uint64_t index;
} __rte_cache_aligned arr_index[MAX_TX_RX_QUEUE];

/* Global sequence number — source of truth for all queues.
 * Initialized to PKT_SEQ_START (1) so item.nseq = 0 on the server
 * always means "never written", avoiding false-positive dedup on new items. */
static uint64_t global_nseq __rte_cache_aligned = PKT_SEQ_START;

/* Per-queue batch reservation.  Each queue grabs send_batch_size values at
 * once via a single FAA on global_nseq, then assigns from [next, limit)
 * locally with no further atomics.  next == limit triggers the next refill. */
static struct nseq_batch {
	uint64_t next;
	uint64_t limit;
} __rte_cache_aligned nseq_batch[MAX_TX_RX_QUEUE];

static uint16_t src_port_start = SRC_PORT_START;
static uint16_t src_port_end   = SRC_PORT_MAX;

static struct queue_port_range {
	uint16_t start;
	uint16_t end;
	uint16_t mask;  /* rte_flow bitmask: 0xFFFF ^ (ports_per_queue-1) */
} __rte_cache_aligned q_port_range[MAX_TX_RX_QUEUE];

#define MAX_FLOW_RULES_PER_QUEUE 256
static struct rte_flow *flow_rules[MAX_TX_RX_QUEUE][MAX_FLOW_RULES_PER_QUEUE];
static uint32_t flow_rule_count[MAX_TX_RX_QUEUE];

/* Feature 2: Retries ------------------------------------------------------- */

static uint64_t retry_timeout_us     = 1000; /* default 1 ms */
static uint8_t  max_retries          = 3;
static uint32_t pending_scale        = 2;    /* max_pending = next_pow2(max_in_flight * pending_scale) */

/* Key space mask for HT workload — derived from --num-keys, or from the
 * compiled-in KEY_NUM_BITS default if --num-keys is not supplied. */
static uint64_t key_mask = ((uint64_t)1 << KEY_NUM_BITS) - 1;

/* YCSB workload selection: 0=A (50/50), 1=B (95/5), 2=C (100/0) */
static uint8_t ycsb_workload = 1;
static const uint8_t ycsb_read_pct[3] = { 50, 95, 100 };
static const char   *ycsb_name[3]     = {
    "A (50% GET / 50% SET)",
    "B (95% GET /  5% SET)",
    "C (100% GET /  0% SET)",
};

/* Key distribution */
static bool   use_zipf    = false;
static double zipf_theta  = 0.99;
static uint64_t retry_timeout_cycles = 0;    /* computed in main() */
static uint32_t max_pending          = 0;    /* computed in main() */

/* Compact retransmit descriptor — no mbuf pointer */
struct pending_req {
	uint64_t nseq;
	uint64_t orig_send_time; /* first TX time — used for RTT */
	uint64_t last_send_time; /* most recent TX time — used for retry timeout */
	uint16_t src_port;
	uint32_t src_ip;
	uint8_t  func_id;
	uint8_t  payload_type;
	uint8_t  retry_count;
	bool     valid;
};

struct pending_fifo {
	uint64_t *ring; /* dynamically allocated, size = max_pending */
	uint32_t  head;
	uint32_t  tail;
};

struct pending_table {
	struct pending_req  *slots; /* dynamically allocated, size = max_pending */
	struct pending_fifo  fifo;
	uint64_t             total_retries;
	uint64_t             retry_exhausted;
	uint64_t             unresolved;
} __rte_cache_aligned pending[MAX_TX_RX_QUEUE];

/* --------------------------------------------------------------------------- */

struct pkt_sample {
	uint64_t nseq;
	uint64_t orig_send_cycles;
	uint64_t rtt_cycles;   /* 0 = lost */
	uint8_t  server_type;
};

static struct container_rrt {
	struct pkt_sample *data;
} __rte_cache_aligned rtt_times[MAX_TX_RX_QUEUE];

/* Total samples per queue (received + lost); arr_index tracks received only. */
static struct arr_total {
	uint64_t index;
} __rte_cache_aligned arr_total[MAX_TX_RX_QUEUE];

static struct ht_stats {
	uint32_t ht_get_success;
	uint32_t ht_get_error;
	uint32_t ht_get_locked;
	uint32_t ht_get_key_not_found;
	uint32_t ht_get_version;
	uint32_t ht_set_success;
	uint32_t ht_set_error;
	uint32_t ht_set_locked;
	uint32_t ht_set_full;
	uint32_t ht_set_version;
	uint32_t ht_set_duplicate; /* server dropped as already-applied nseq */
} __rte_cache_aligned ht_stats[MAX_TX_RX_QUEUE];

struct xorshift64_state {
    uint64_t a;
} __rte_cache_aligned rand_state[MAX_TX_RX_QUEUE];

uint64_t xorshift64(struct xorshift64_state *state)
{
	uint64_t x = state->a;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return state->a = x;
}

/*struct xorshift64_state *rand_state;*/

/* ---- Zipf sampler -------------------------------------------------------- */

static struct zipf_params g_zipf;

static double zeta_sum(uint64_t n, double theta)
{
    double z = 0.0;
    for (uint64_t i = 1; i <= n; i++)
        z += pow((double)i, -theta);
    return z;
}

void zipf_init(struct zipf_params *zp, uint64_t n, double theta)
{
    if (n > 100000)
        printf("Zipf: computing zeta(%lu, %.4f)...", n, theta);
    fflush(stdout);
    zp->n      = n;
    zp->theta  = theta;
    zp->zeta_n = zeta_sum(n, theta);
    zp->zeta_2 = 1.0 + pow(0.5, theta);
    zp->alpha  = 1.0 / (1.0 - theta);
    zp->eta    = (1.0 - pow(2.0 / (double)n, 1.0 - theta))
                 / (1.0 - zp->zeta_2 / zp->zeta_n);
    if (n > 100000)
        printf(" done (zeta_n=%.4f)\n", zp->zeta_n);
}

uint64_t zipf_sample(const struct zipf_params *zp, uint64_t rng_val)
{
    /* Map rng_val in [0, UINT64_MAX] to u in (0, 1) */
    double u  = ((double)rng_val + 0.5) / ((double)UINT64_MAX + 1.0);
    double uz = u * zp->zeta_n;
    if (uz < 1.0)
        return 0;
    if (uz < 1.0 + pow(0.5, zp->theta))
        return 1;
    uint64_t k = (uint64_t)((double)zp->n * pow(zp->eta * u - zp->eta + 1.0, zp->alpha));
    return k < zp->n ? k : zp->n - 1;
}

/* -------------------------------------------------------------------------- */

static inline uint64_t next_key(uint8_t q_id)
{
    uint64_t r = xorshift64(&rand_state[q_id]);
    if (use_zipf)
        return zipf_sample(&g_zipf, r);
    return r & key_mask;
}

static char *dma_data = NULL;

/* dnetperf.c: simple implementation of netperf on DPDK */

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

void print_ip_u32(uint32_t ip)
{
	printf("%d.%d.%d.%d", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

static int str_to_long(const char *str, long *val)
{
	char *endptr;

	*val = strtol(str, &endptr, 10);
	if (endptr == str || (*endptr != '\0' && *endptr != '\n') ||
	    ((*val == LONG_MIN || *val == LONG_MAX) && errno == ERANGE))
		return -EINVAL;
	return 0;
}

int comp(const void *a, const void *b) {
	uint64_t ua = *((uint64_t *)a);
	uint64_t ub = *((uint64_t *)b);

	if (ua > ub) return 1;
	if (ua < ub) return -1;

	return 0;
}

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool, unsigned int n_queues)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = n_queues, tx_rings = n_queues;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf *txconf;

	printf("initializing with %u queues\n", n_queues);

	if (!rte_eth_dev_is_valid_port(port)){
		printf("port valid\n");
		return -1;

	}

	rte_eth_dev_info_get(port, &dev_info);

	/* Clamp requested offloads/RSS to what this device actually
	 * supports (e.g. virtio_user advertises neither IPV4_CKSUM nor
	 * RSS, unlike the physical NICs this config was tuned for). */
	port_conf.rxmode.offloads &= dev_info.rx_offload_capa;
	port_conf.txmode.offloads &= dev_info.tx_offload_capa;
	if ((dev_info.flow_type_rss_offloads & port_conf.rx_adv_conf.rss_conf.rss_hf) == 0) {
		port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
		port_conf.rx_adv_conf.rss_conf.rss_hf = 0;
	}

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0){
		printf("port config\n");
		return retval;

	}

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up RX queues */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                                        rte_eth_dev_socket_id(port), NULL,
                                        mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Enable TX offloading */
	txconf = &dev_info.default_txconf;

	/* Allocate and set up TX queues */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                                        rte_eth_dev_socket_id(port), txconf);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	rte_eth_macaddr_get(port, &my_eth);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			my_eth.addr_bytes[0], my_eth.addr_bytes[1],
			my_eth.addr_bytes[2], my_eth.addr_bytes[3],
			my_eth.addr_bytes[4], my_eth.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * Send out an arp.
 */
static void send_arp(uint16_t op, struct rte_ether_addr dst_eth, uint32_t dst_ip)
{
	struct rte_mbuf *buf;
	char *buf_ptr;
	struct rte_ether_hdr *eth_hdr;
	struct rte_arp_hdr *a_hdr;
	int nb_tx;

	buf = rte_pktmbuf_alloc(tx_mbuf_pool);
	if (buf == NULL)
		printf("error allocating arp mbuf\n");

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(&dst_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

	/* arp header */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_arp_hdr));
	a_hdr = (struct rte_arp_hdr *) buf_ptr;
	a_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
	a_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
	a_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
	a_hdr->arp_plen = 4;
	a_hdr->arp_opcode = rte_cpu_to_be_16(op);

	rte_ether_addr_copy(&my_eth, &a_hdr->arp_data.arp_sha);
	a_hdr->arp_data.arp_sip = rte_cpu_to_be_32(SRC_IP_START);
	rte_ether_addr_copy(&dst_eth, &a_hdr->arp_data.arp_tha);
	a_hdr->arp_data.arp_tip = rte_cpu_to_be_32(dst_ip);

	nb_tx = rte_eth_tx_burst(dpdk_port, 0, &buf, 1);
	if (unlikely(nb_tx != 1)) {
		printf("error: could not send arp packet\n");
	}
}

/*
 * Validate this ethernet header. Return true if this packet is for higher
 * layers, false otherwise.
 */
static bool check_eth_hdr(struct rte_mbuf *buf)
{
	struct rte_ether_hdr *ptr_mac_hdr;
	struct rte_arp_hdr *a_hdr;

	ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
	if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth) &&
			!rte_is_broadcast_ether_addr(&ptr_mac_hdr->dst_addr)) {
		/* packet not to our ethernet addr */
		return false;
	}

	if (ptr_mac_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
		/* reply to ARP if necessary */
		a_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_arp_hdr *,
				sizeof(struct rte_ether_hdr));
		if (a_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REQUEST)
				&& a_hdr->arp_data.arp_tip == rte_cpu_to_be_32(SRC_IP_START))
			send_arp(RTE_ARP_OP_REPLY, a_hdr->arp_data.arp_sha,
					rte_be_to_cpu_32(a_hdr->arp_data.arp_sip));
		return false;
	}

	if (ptr_mac_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
		/* packet not IPv4 */
		return false;

	return true;
}

/*
 * Return true if this IP packet is to us and contains a UDP packet,
 * false otherwise.
 */
static bool check_ip_hdr(struct rte_mbuf *buf)
{
	struct rte_ipv4_hdr *ipv4_hdr;

	ipv4_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_ipv4_hdr *,
			RTE_ETHER_HDR_LEN);

	for (uint32_t i = SRC_IP_START; i <= SRC_IP_MAX; i++) {
		
		if (ipv4_hdr->dst_addr == rte_cpu_to_be_32(i)
				&& ipv4_hdr->next_proto_id == IPPROTO_UDP) {
			
			return true;
		}
	}
	
	return false;
}

/*
 * Resolve mac
 */

void get_server_mac(struct rte_ether_addr *server_eth) {
	struct rte_mbuf *bufs[BURST_SIZE];
	struct rte_mbuf *buf;
	struct rte_ether_hdr *ptr_mac_hdr;
	struct rte_arp_hdr *a_hdr;
	int nb_rx, nb_tx, i;

	/* get the mac address of the server via ARP */
	while (true) {
		send_arp(RTE_ARP_OP_REQUEST, broadcast_mac, server_ip);
		sleep(1);

		nb_rx = rte_eth_rx_burst(dpdk_port, 0, bufs, BURST_SIZE);
		if (nb_rx == 0) {
			printf("No ARP reply\n");
			continue;
		}
		printf("ARP reply found\n");

		for (i = 0; i < nb_rx; i++) {
			buf = bufs[i];

			ptr_mac_hdr = rte_pktmbuf_mtod(buf, struct rte_ether_hdr *);
			if (!rte_is_same_ether_addr(&ptr_mac_hdr->dst_addr, &my_eth)) {
					/* packet not to our ethernet addr */
					printf("Same address!\n");
					continue;
			}

			if (ptr_mac_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
				/* this is an ARP */
				a_hdr = rte_pktmbuf_mtod_offset(buf, struct rte_arp_hdr *,
						sizeof(struct rte_ether_hdr));
				if (a_hdr->arp_opcode == rte_cpu_to_be_16(RTE_ARP_OP_REPLY) &&
						rte_is_same_ether_addr(&a_hdr->arp_data.arp_tha, &my_eth) &&
						a_hdr->arp_data.arp_tip == rte_cpu_to_be_32(SRC_IP_START)) {
					/* got a response from server! */
					rte_ether_addr_copy(&a_hdr->arp_data.arp_sha, server_eth);
					return;
				}
			}
		}
	}
}

void gen_ht_get_payload(req_pkt_t *req_pkt, uint8_t q_id, bool is_deterministic)
{
	ht_entry_t entry;
	entry.cmd    = 0;
	entry.status = 0;
	entry.key    = is_deterministic ? 314 : next_key(q_id);
	*(ht_entry_t *)(req_pkt->data) = entry;
}

void gen_ht_set_payload(req_pkt_t *req_pkt, uint8_t q_id, bool is_deterministic)
{
	ht_entry_t entry;
	entry.cmd    = 1;
	entry.status = 0;
	if (is_deterministic) {
		entry.key   = 314;
		entry.value = 315;
	} else {
		entry.key   = next_key(q_id);
		entry.value = entry.key + 1;
	}
	*(ht_entry_t *)(req_pkt->data) = entry;
}

void gen_ht_payload(void *ptr_to_pkt, uint8_t q_id)
{
	req_pkt_t *req_pkt = (req_pkt_t *)ptr_to_pkt;
	req_pkt->server_type = 0;

	if (xorshift64(&rand_state[q_id]) % 100 < ycsb_read_pct[ycsb_workload]) {
		gen_ht_get_payload(req_pkt, q_id, false);
	} else {
		req_pkt->udp.dest = rte_cpu_to_be_16(HT_SET_DST_PORT);
		gen_ht_set_payload(req_pkt, q_id, false);
	}
}

void gen_bpt_payload(void *ptr_to_pkt, uint8_t q_id) {
	req_pkt_t *req_pkt = (req_pkt_t *)ptr_to_pkt;
	bpt_entry_t *entry = (bpt_entry_t *)req_pkt->data;

	req_pkt->server_type = 0;

	entry->key = 6;
}

/* Gen get payload */
void gen_get_payload(void *ptr_to_pkt, uint8_t q_id) {
	req_pkt_t *req_pkt = (req_pkt_t *)ptr_to_pkt;
	get_set_entry_t *entry = (get_set_entry_t *)(req_pkt->data);

	req_pkt->server_type = 0;

	uint64_t test = xorshift64(&rand_state[q_id]);
	req_pkt->func_id = test % num_funcs;

	entry->op_type = 0;
	entry->mr_offset = 16;
	entry->data_size = DMA_RW_SIZE;
}

/* Gen set payload */
void gen_set_payload(void *ptr_to_pkt, uint8_t q_id) {
	req_pkt_t *req_pkt = (req_pkt_t *)ptr_to_pkt;
	get_set_entry_t *entry = (get_set_entry_t *)req_pkt->data;

	req_pkt->server_type = 0;

	uint64_t test = xorshift64(&rand_state[q_id]);
	req_pkt->func_id = test % num_funcs;

	entry->op_type = 1;
	entry->mr_offset = 16;
	entry->data_size = DMA_RW_SIZE;
	
	// Copy data to write with dma write op into packet buffer
	strncpy((char *)req_pkt->data + sizeof(get_set_entry_t), dma_data, DMA_RW_SIZE);
}

/* Gen linked list traversal payload */
void gen_linked_list_payload(void *ptr_to_pkt, uint8_t q_id) {
	req_pkt_t *req_pkt = (req_pkt_t *)ptr_to_pkt;

	req_pkt->server_type = 0;

	// Walk 4 nodes
	*(uint32_t *)req_pkt->data = 4;
}

/* Extract timestamp from payload */
static inline void collect_stats(req_pkt_t *reply_pkt, uint64_t time_recv_cycle,
				  uint64_t orig_send_time, uint8_t qid) {
	if (time_recv_cycle < warmup_end_cycles)
		return;
	uint64_t idx = arr_index[qid].index++;
	struct pkt_sample *s = &rtt_times[qid].data[idx];
	s->nseq             = reply_pkt->nseq;
	s->orig_send_cycles = orig_send_time;
	s->rtt_cycles       = time_recv_cycle - orig_send_time;
	s->server_type      = reply_pkt->server_type;
	arr_total[qid].index++;

	// Collect hashtable stats
	if (payload_type == PAYLOAD_TYPE_HT) {
		ht_entry_t *entry = (ht_entry_t *)reply_pkt->data;
		
		if (entry->cmd == 0) {
			if (entry->status == HT_SUCCESS && entry->value != entry->key + 1) {
				// printf("Key %lu, value %lu\n", entry->key, entry->value);
			}

			switch(entry->status) {
				case HT_SUCCESS:
					ht_stats[qid].ht_get_success++;
					break;
				case HT_ERR_KEY_NOT_FOUND:
					ht_stats[qid].ht_get_key_not_found++;
					break;
				case HT_ERR_LOCKED:
					ht_stats[qid].ht_get_locked++;
					break;
				case HT_ERR_VERSION_UPDATE:
					ht_stats[qid].ht_get_version++;
					break;
				case HT_ERR_FATAL:
					ht_stats[qid].ht_get_error++;
					break;
				default:
					printf("Unknown status %d\n", entry->status);
					break;
			}
		}
		else {
			switch(entry->status) {
				case HT_SUCCESS:
					ht_stats[qid].ht_set_success++;
					break;
				case HT_ERR_LOCKED:
					ht_stats[qid].ht_set_locked++;
					break;
				case HT_ERR_VERSION_UPDATE:
					ht_stats[qid].ht_set_version++;
					break;
				case HT_ERR_FULL:
					ht_stats[qid].ht_set_full++;
					break;
				case HT_ERR_DUPLICATE:
					ht_stats[qid].ht_set_duplicate++;
					break;
				case HT_ERR_FATAL:
					ht_stats[qid].ht_set_error++;
					break;
				default:
					printf("Unknown status %d\n", entry->status);
					break;
			}
		}
	}
}

void report_stats() {
	uint64_t included_samples = 0;
	uint64_t total_cycles = 0;

	for (uint64_t j = 0; j < num_queues; j++) {
		for (uint64_t i = 0; i < arr_index[j].index; i++) {
			total_cycles += rtt_times[j].data[i].rtt_cycles;
			diff_times[included_samples++] = rtt_times[j].data[i].rtt_cycles;
		}
	}
	
	// Measure p50 and p99 latency 
	qsort(diff_times, included_samples, sizeof(uint64_t), comp);
	uint64_t p50_cycles = diff_times[(uint64_t)(included_samples * 0.5)];
	uint64_t p99_cycles = diff_times[(uint64_t)(included_samples * 0.99)];

	printf("mean latency (us): %f\n", (float) total_cycles *
		1000 * 1000 / (included_samples * rte_get_timer_hz()));
	printf("median latency (us): %f\n", (p50_cycles * 1000.0 * 1000.0) / rte_get_timer_hz());
	printf("99th latency (us): %f\n", (p99_cycles * 1000.0 * 1000.0) / rte_get_timer_hz());

	{
		uint64_t total_retries = 0, total_exhausted = 0, total_unresolved = 0;
		for (int qi = 0; qi < (int)num_queues; qi++) {
			total_retries    += pending[qi].total_retries;
			total_exhausted  += pending[qi].retry_exhausted;
			total_unresolved += pending[qi].unresolved;
		}
		printf("Retries:               %lu\n", total_retries);
		printf("Retry exhausted (lost):%lu\n", total_exhausted);
		printf("Unresolved at end:     %lu\n", total_unresolved);
	}

	if (payload_type == PAYLOAD_TYPE_HT) {
		uint64_t total_gets = 0;
		uint64_t total_sets = 0;
		uint64_t ht_get_success = 0;
		uint64_t ht_get_error = 0;
		uint64_t ht_get_locked = 0;
		uint64_t ht_get_key_not_found = 0;
		uint64_t ht_get_version = 0;
		uint64_t ht_set_success = 0;
		uint64_t ht_set_error = 0;
		uint64_t ht_set_locked = 0;
		uint64_t ht_set_full = 0;
		uint64_t ht_set_version = 0;
		uint64_t ht_set_duplicate = 0;

		for (int i = 0; i < num_queues; i++) {
			ht_get_success += ht_stats[i].ht_get_success;
			ht_get_error += ht_stats[i].ht_get_error;
			ht_get_locked += ht_stats[i].ht_get_locked;
			ht_get_key_not_found += ht_stats[i].ht_get_key_not_found;
			ht_get_version += ht_stats[i].ht_get_version;
			ht_set_success += ht_stats[i].ht_set_success;
			ht_set_error += ht_stats[i].ht_set_error;
			ht_set_locked += ht_stats[i].ht_set_locked;
			ht_set_full += ht_stats[i].ht_set_full;
			ht_set_version += ht_stats[i].ht_set_version;
			ht_set_duplicate += ht_stats[i].ht_set_duplicate;
		}

		total_gets = ht_get_success + ht_get_error + ht_get_locked + ht_get_key_not_found + ht_get_version;
		total_sets = ht_set_success + ht_set_error + ht_set_locked + ht_set_full + ht_set_version + ht_set_duplicate;

		printf("\nGETS: %lu\n", total_gets);
		printf("GET SUCCESS: %lu\n", ht_get_success);
		printf("GET BUCKET LOCKED: %lu\n", ht_get_locked);
		printf("GET KEY NOT FOUND: %lu\n", ht_get_key_not_found);
		printf("GET VERSION CHECK FAILED: %lu\n", ht_get_version);
		printf("GET ERROR: %lu\n\n", ht_get_error);

		printf("SETS: %lu\n", total_sets);
		printf("SET SUCCESS: %lu\n", ht_set_success);
		printf("SET BUCKET LOCK FAILED: %lu\n", ht_set_locked);
		printf("SET BUCKET FULL: %lu\n", ht_set_full);
		printf("SET VERSION UPDATE FAILED: %lu\n", ht_set_version);
		printf("SET DUPLICATE (server dedup drop): %lu\n", ht_set_duplicate);
		printf("SET ERROR: %lu\n", ht_set_error);
	}
}

void dump_stats() {
	FILE *fp;
	double hz = (double)rte_get_timer_hz();
	uint64_t row = 0;

	if ((fp = fopen(output_filename, "w")) == NULL) {
		printf("Error opening file: %s\n", output_filename);
		return;
	}

	fprintf(fp, "pkt_sqn,send_time (us),rtt (us),server_type\n");

	for (uint64_t q = 0; q < num_queues; q++) {
		for (uint64_t i = 0; i < arr_total[q].index; i++) {
			struct pkt_sample *s = &rtt_times[q].data[i];
			uint64_t ref = s->orig_send_cycles > warmup_end_cycles
			               ? s->orig_send_cycles - warmup_end_cycles : 0;
			double send_us = (double)ref * 1e6 / hz;
			double rtt_us  = (double)s->rtt_cycles * 1e6 / hz;
			fprintf(fp, "%lu,%f,%f,%d\n", row++, send_us, rtt_us, s->server_type);
		}
	}

	fclose(fp);
}

/* Add timestamp in the packets */
void add_timestamp(struct rte_mbuf **bufs, int burst_size) {
	struct rte_mbuf *buf;
	req_pkt_t *req_pkt;
	uint64_t timestamp = rte_get_timer_cycles();

	for (int i = 0; i < burst_size; i++) {
		buf = bufs[i];

		req_pkt = rte_pktmbuf_mtod(buf, req_pkt_t *);

		req_pkt->timestamp = timestamp;
	}
}

/*
 * Allocate a packet
 */

struct rte_mbuf *allocate_pkt(struct rte_ether_addr *server_eth, int q_id) {
	struct rte_mbuf *buf;
	struct rte_ether_hdr *eth_hdr;
	struct rte_ipv4_hdr *ipv4_hdr;
	struct rte_udp_hdr *rte_udp_hdr;
	char *buf_ptr;
	

	buf = rte_pktmbuf_alloc(tx_mbuf_pool);
	if (buf == NULL)
		printf("error allocating tx mbuf\n");

	/* ethernet header */
	buf_ptr = rte_pktmbuf_append(buf, RTE_ETHER_HDR_LEN);
	eth_hdr = (struct rte_ether_hdr *) buf_ptr;

	rte_ether_addr_copy(&my_eth, &eth_hdr->src_addr);
	rte_ether_addr_copy(server_eth, &eth_hdr->dst_addr);
	eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* IPv4 header */
	buf_ptr = rte_pktmbuf_append(buf, sizeof(struct rte_ipv4_hdr));
	ipv4_hdr = (struct rte_ipv4_hdr *) buf_ptr;
	ipv4_hdr->version_ihl = 0x45;
	ipv4_hdr->type_of_service = 0;
	ipv4_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
			sizeof(struct rte_udp_hdr) + payload_len);
	ipv4_hdr->packet_id = 0;
	ipv4_hdr->fragment_offset = 0;
	ipv4_hdr->time_to_live = 64;
	ipv4_hdr->next_proto_id = IPPROTO_UDP;
	ipv4_hdr->hdr_checksum = 0;
		
	ipv4_hdr->src_addr = rte_cpu_to_be_32(get_next_ip[q_id].ip++);
		
	ipv4_hdr->dst_addr = rte_cpu_to_be_32(server_ip);

	/* UDP header + data */
	buf_ptr = rte_pktmbuf_append(buf,
			sizeof(struct rte_udp_hdr) + payload_len);
	rte_udp_hdr = (struct rte_udp_hdr *) buf_ptr;
	rte_udp_hdr->src_port = rte_cpu_to_be_16(src_port_next[q_id].port++);
	rte_udp_hdr->dst_port = rte_cpu_to_be_16(server_port);
	rte_udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr)
			+ payload_len);
	rte_udp_hdr->dgram_cksum = 0;

	memset(buf_ptr + sizeof(struct rte_udp_hdr), 0, payload_len);

	void *ptr_to_pkt = rte_pktmbuf_mtod(buf, void *);

	if (payload_type == PAYLOAD_TYPE_GET)
		gen_get_payload(ptr_to_pkt, q_id);
	else if (payload_type == PAYLOAD_TYPE_SET)
		gen_set_payload(ptr_to_pkt, q_id);
	else if (payload_type == PAYLOAD_TYPE_LIST)
		gen_linked_list_payload(ptr_to_pkt, q_id);
	else if (payload_type == PAYLOAD_TYPE_HT)
		gen_ht_payload(ptr_to_pkt, q_id);
	else if (payload_type == PAYLOAD_TYPE_BPT)
		gen_bpt_payload(ptr_to_pkt, q_id);

	if (unlikely(nseq_batch[q_id].next == nseq_batch[q_id].limit)) {
		nseq_batch[q_id].next  = __atomic_fetch_add(&global_nseq,
		                                             (uint64_t)send_batch_size,
		                                             __ATOMIC_RELAXED);
		nseq_batch[q_id].limit = nseq_batch[q_id].next + send_batch_size;
	}
	((req_pkt_t *)ptr_to_pkt)->nseq = nseq_batch[q_id].next++;

	buf->l2_len = RTE_ETHER_HDR_LEN;
	buf->l3_len = sizeof(struct rte_ipv4_hdr);
	buf->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
	
	if (get_next_ip[q_id].ip == (SRC_IP_MAX + 1))
		get_next_ip[q_id].ip = SRC_IP_START;

	if (src_port_next[q_id].port > q_port_range[q_id].end)
		src_port_next[q_id].port = q_port_range[q_id].start;

	return buf;
}

static inline void pending_insert(uint8_t q_id, uint64_t nseq, uint64_t send_time,
				   uint16_t src_port, uint32_t src_ip,
				   uint8_t func_id, uint8_t ptype)
{
	struct pending_table *pt = &pending[q_id];
	uint32_t idx = (uint32_t)(nseq & (max_pending - 1));
	struct pending_req *slot = &pt->slots[idx];

	slot->nseq           = nseq;
	slot->orig_send_time = send_time;
	slot->last_send_time = send_time;
	slot->src_port       = src_port;
	slot->src_ip         = src_ip;
	slot->func_id        = func_id;
	slot->payload_type   = ptype;
	slot->retry_count    = 0;
	slot->valid          = true;

	pt->fifo.ring[pt->fifo.tail & (max_pending - 1)] = nseq;
	pt->fifo.tail++;
}

static inline uint64_t pending_remove(uint8_t q_id, uint64_t nseq)
{
	struct pending_table *pt = &pending[q_id];
	uint32_t idx = (uint32_t)(nseq & (max_pending - 1));
	struct pending_req *slot = &pt->slots[idx];

	if (!slot->valid || slot->nseq != nseq)
		return 0;

	slot->valid = false;
	return slot->orig_send_time;
}

static void pending_check_timeouts(uint8_t q_id, uint64_t now,
				    struct rte_ether_addr *srv_eth)
{
	struct pending_table *pt = &pending[q_id];

	while (pt->fifo.head != pt->fifo.tail) {
		uint64_t head_nseq = pt->fifo.ring[pt->fifo.head & (max_pending - 1)];
		uint32_t idx = (uint32_t)(head_nseq & (max_pending - 1));
		struct pending_req *slot = &pt->slots[idx];

		/* Skip stale entries already removed by pending_remove */
		if (!slot->valid || slot->nseq != head_nseq) {
			pt->fifo.head++;
			continue;
		}

		/* Head not yet expired — nothing older can be expired */
		if (now - slot->last_send_time < retry_timeout_cycles)
			break;

		/* Pop from FIFO */
		pt->fifo.head++;

		if (slot->retry_count < max_retries) {
			/* Build retry packet using original src port/IP */
			uint16_t saved_port = src_port_next[q_id].port;
			uint32_t saved_ip   = get_next_ip[q_id].ip;
			src_port_next[q_id].port = slot->src_port;
			get_next_ip[q_id].ip     = slot->src_ip;

			struct rte_mbuf *retry_buf = allocate_pkt(srv_eth, q_id);

			/* Restore normal cycling state */
			src_port_next[q_id].port = saved_port;
			get_next_ip[q_id].ip     = saved_ip;

			if (retry_buf != NULL) {
				req_pkt_t *req = rte_pktmbuf_mtod(retry_buf, req_pkt_t *);
				req->nseq      = head_nseq; /* preserve original nseq */
				req->timestamp = now;

				if (rte_eth_tx_burst(dpdk_port, q_id, &retry_buf, 1) == 0)
					rte_pktmbuf_free(retry_buf);
			}

			slot->retry_count++;
			slot->last_send_time = now;
			pt->total_retries++;

			/* Re-push to FIFO tail for next timeout window */
			pt->fifo.ring[pt->fifo.tail & (max_pending - 1)] = head_nseq;
			pt->fifo.tail++;
		} else {
			slot->valid = false;
			pt->retry_exhausted++;
		}
	}
}

static int open_loop(void *arg) {
	struct rte_mbuf *bufs[recv_batch_size];
	struct rte_mbuf *buf;
	req_pkt_t *reply_pkt;
	uint64_t start_time, end_time, time_received, trigger_time;
	int32_t nb_tx, nb_rx, i;
	int send_burst = send_batch_size;
	uint64_t packet_delay_cycle = 0;
	uint64_t pps_current = 0, pps_end = 0, pps_step = 0;
	bool climb_down = false;
	bool gen_unlimited = false;
	uint64_t deadline;
	struct open_loop_arg *args = (struct open_loop_arg *)arg;
	uint8_t q_id = args->queue_id;
	struct rte_ether_addr *server_eth = args->eth_addr;
	bool is_print_pkt_sz = false;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	if (rte_eth_dev_socket_id(dpdk_port) != (int)rte_socket_id())
        printf("WARNING, port %u (socket %d) is on remote NUMA node to polling thread (socket %d).\n\t"
               "Performance will not be optimal.\n", dpdk_port, rte_eth_dev_socket_id(dpdk_port), rte_socket_id());

	printf("\nCore %d\tQueue: %d\n",
			rte_lcore_id(), q_id);

	/* Initialize client flows */
	get_next_ip[q_id].ip = SRC_IP_START;
	src_port_next[q_id].port = q_port_range[q_id].start;
	nseq_batch[q_id].next  = 0;
	nseq_batch[q_id].limit = 0; /* next == limit triggers batch refill on first call */

	/* Initialize pending retry table for this queue */
	pending[q_id].fifo.head      = 0;
	pending[q_id].fifo.tail      = 0;
	pending[q_id].total_retries  = 0;
	pending[q_id].retry_exhausted = 0;
	pending[q_id].unresolved     = 0;
	if (!gen_unlimited) {
		memset(pending[q_id].slots, 0, max_pending * sizeof(struct pending_req));
		memset(pending[q_id].fifo.ring, 0, max_pending * sizeof(uint64_t));
	}

	// If seconds is 0, then generate packets indefinitely
	if (seconds == 0) {
		gen_unlimited = true;
	}
	
	pps_current = send_pps_start / num_queues;
	pps_end = send_pps_end / num_queues;
	pps_step = send_pps_step / num_queues;

	// Inter batch delay
	if (pps_current) {
		packet_delay_cycle = rte_get_timer_hz() * (send_batch_size / (double)pps_current);
	}

	start_time = rte_get_timer_cycles();
	send_start_time = start_time;
	warmup_end_cycles = send_start_time + (uint64_t)warmup_seconds * rte_get_timer_hz();
	trigger_time = rte_get_timer_cycles();
	
	deadline = start_time;
	uint64_t tx_num = 0;

	int packets_to_send = send_batch_size;
	int deadline_count = 0;
	struct rte_mbuf *send_tx_bufs[send_burst];
	int tx_pending = 0;      /* unsent mbufs remaining from partial TX */
	int tx_pending_off = 0;  /* offset into send_tx_bufs for retry */
	while (gen_unlimited || rte_get_timer_cycles() < (start_time + seconds * rte_get_timer_hz())) {
		if (!climb_down && pps_current < pps_end && rte_get_timer_cycles() > (trigger_time + send_pps_trigger_time * rte_get_timer_hz())) {
			pps_current += pps_step;
			packet_delay_cycle = rte_get_timer_hz() * (send_batch_size / (double)pps_current);
			trigger_time = rte_get_timer_cycles();
		}


		if (climb_down && pps_current > send_pps_start
			&& rte_get_timer_cycles() > (trigger_time + send_pps_trigger_time * rte_get_timer_hz())) {
			
			pps_current -= pps_step;
			packet_delay_cycle = rte_get_timer_hz() * (send_batch_size / (double)pps_current);
			trigger_time = rte_get_timer_cycles();
		}

		/* Drain TX queue in every 100 ms */
		
		if (tx_pending > 0) {
			/* Retry sending leftover mbufs from a previous partial TX */
			nb_tx = rte_eth_tx_burst(dpdk_port, q_id,
						 &send_tx_bufs[tx_pending_off], tx_pending);
			port_statistics[q_id].tx += nb_tx;

			if (!gen_unlimited) {
				for (int n = 0; n < nb_tx; n++) {
					req_pkt_t *req = rte_pktmbuf_mtod(
						send_tx_bufs[tx_pending_off + n], req_pkt_t *);
					pending_insert(q_id, req->nseq, req->timestamp,
						       rte_be_to_cpu_16(req->udp.source),
						       rte_be_to_cpu_32(req->ip.saddr),
						       req->func_id, payload_type);
				}
			}

			tx_pending_off += nb_tx;
			tx_pending     -= nb_tx;

			if (tx_pending == 0) {
				deadline += packet_delay_cycle;
				packets_to_send = send_batch_size;
			}
		} else if (rte_get_timer_cycles() >= deadline) {
			for (int n_pkt = 0; n_pkt < packets_to_send; n_pkt++) {
				buf = allocate_pkt(server_eth, q_id);
				send_tx_bufs[n_pkt] = buf;
				if (!is_print_pkt_sz) {
					printf("Packet size: %u\n", rte_pktmbuf_pkt_len(buf));
					is_print_pkt_sz = true;
				}
			}

			/*
			 * send packet: need some amout of sleep after this functions
			 * Otherwise rte_eth_tx_burst can't get time to free buffer and dies
			 *
			 * Alternative: https://stackoverflow.com/questions/69169224/rte-eth-tx-burst-descriptor-mbuf-management-guarantees-vs-free-thresholds
			 */
			if (!gen_unlimited)
				assert(port_statistics[q_id].tx + packets_to_send < (MAX_SAMPLES / num_queues));

			add_timestamp(send_tx_bufs, packets_to_send);

			nb_tx = rte_eth_tx_burst(dpdk_port, q_id, send_tx_bufs, packets_to_send);
			port_statistics[q_id].tx += nb_tx;

			if (!gen_unlimited) {
				for (int n = 0; n < nb_tx; n++) {
					req_pkt_t *req = rte_pktmbuf_mtod(send_tx_bufs[n], req_pkt_t *);
					pending_insert(q_id, req->nseq, req->timestamp,
						       rte_be_to_cpu_16(req->udp.source),
						       rte_be_to_cpu_32(req->ip.saddr),
						       req->func_id, payload_type);
				}
			}

			if (unlikely(nb_tx < packets_to_send)) {
				/* Keep unsent mbufs for retry on next iteration */
				tx_pending_off = nb_tx;
				tx_pending     = packets_to_send - nb_tx;
			} else {
				deadline += packet_delay_cycle;
				packets_to_send = send_batch_size;
			}
		}
			
		nb_rx = rte_eth_rx_burst(dpdk_port, q_id, bufs, BURST_SIZE > send_burst ? BURST_SIZE : send_burst);
		time_received = rte_get_timer_cycles();
		
		for (i = 0; i < nb_rx; i++) {
			buf = bufs[i];
			
			if (!check_eth_hdr(buf)) {
				/*printf("error: eth header check failed\n");*/
				rte_pktmbuf_free(buf);
				continue;
			}

			if (!check_ip_hdr(buf)) {
				/*printf("error: ipv4 header check failed\n");*/
				rte_pktmbuf_free(buf);
				continue;
			}

			/* ptr to packet data */
			reply_pkt = rte_pktmbuf_mtod(buf, req_pkt_t *);

			/* Verify response dst_port belongs to this queue's port sub-range */
			uint16_t resp_dst_port = rte_be_to_cpu_16(reply_pkt->udp.dest);
			if (resp_dst_port < q_port_range[q_id].start ||
			    resp_dst_port > q_port_range[q_id].end)
				port_statistics[q_id].wrong_queue++;

			if (!gen_unlimited) {
				uint64_t orig_send_time = pending_remove(q_id, reply_pkt->nseq);
				if (orig_send_time != 0) {
					collect_stats(reply_pkt, time_received, orig_send_time, q_id);
					port_statistics[q_id].unique_rx++;
					if (reply_pkt->server_type == SERVER_TYPE_HOST)
						port_statistics[q_id].host_rx++;
					else
						port_statistics[q_id].dpu_rx++;
				}
			} else {
				port_statistics[q_id].unique_rx++;
				if (reply_pkt->server_type == SERVER_TYPE_HOST)
					port_statistics[q_id].host_rx++;
				else
					port_statistics[q_id].dpu_rx++;
			}

			port_statistics[q_id].rx++;

			rte_pktmbuf_free(buf);
		}
		
		if (!gen_unlimited)
			pending_check_timeouts(q_id, rte_get_timer_cycles(), server_eth);

		if (loadgen_type == LOADGEN_TYPE_LADDER && pps_current == pps_end && !climb_down) {
			climb_down = true;
		}
	}

	/* Drain remaining pending entries as unresolved losses */
	if (!gen_unlimited) {
		struct pending_table *pt = &pending[q_id];
		while (pt->fifo.head != pt->fifo.tail) {
			uint64_t head_nseq = pt->fifo.ring[pt->fifo.head & (max_pending - 1)];
			uint32_t idx = (uint32_t)(head_nseq & (max_pending - 1));
			struct pending_req *slot = &pt->slots[idx];
			if (slot->valid && slot->nseq == head_nseq) {
				if (slot->orig_send_time >= warmup_end_cycles) {
					uint64_t idx = arr_total[q_id].index++;
					struct pkt_sample *s = &rtt_times[q_id].data[idx];
					s->nseq             = slot->nseq;
					s->orig_send_cycles = slot->orig_send_time;
					s->rtt_cycles       = 0;
					s->server_type      = 0;
				}
				slot->valid = false;
				pt->unresolved++;
			}
			pt->fifo.head++;
		}
	}

	send_end_time = rte_get_timer_cycles();

	return 0;
}

static inline int is_power_of_two(uint32_t n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

static uint32_t next_pow2(uint64_t v)
{
	if (v == 0) return 1;
	v--;
	v |= v >> 1; v |= v >> 2; v |= v >> 4;
	v |= v >> 8; v |= v >> 16; v |= v >> 32;
	return (uint32_t)(v + 1);
}

/*
 * Compute per-queue source port sub-ranges from src_port_start/src_port_end.
 * Must be called after num_queues is set.
 */
static void compute_queue_port_ranges(void)
{
	uint32_t total_ports = (uint32_t)(src_port_end - src_port_start + 1);

	if (src_port_end <= src_port_start)
		rte_exit(EXIT_FAILURE, "src-port-end (%u) must be > src-port-start (%u)\n",
		         src_port_end, src_port_start);

	if (total_ports < num_queues)
		rte_exit(EXIT_FAILURE, "Port range (%u ports) too small for %u queues\n",
		         total_ports, num_queues);

	if (total_ports % num_queues != 0)
		rte_exit(EXIT_FAILURE, "Port range (%u ports) must be divisible by num_queues (%u)\n",
		         total_ports, num_queues);

	uint16_t ports_per_queue = total_ports / num_queues;

	int use_mask = is_power_of_two(ports_per_queue) &&
	               (src_port_start % ports_per_queue == 0);

	if (!use_mask)
		printf("Note: ports_per_queue=%u is not power-of-2 aligned; "
		       "using per-port flow rules (Option B)\n", ports_per_queue);

	if (!use_mask && ports_per_queue > MAX_FLOW_RULES_PER_QUEUE)
		rte_exit(EXIT_FAILURE,
		         "ports_per_queue=%u exceeds MAX_FLOW_RULES_PER_QUEUE=%d; "
		         "use a power-of-2 aligned port range\n",
		         ports_per_queue, MAX_FLOW_RULES_PER_QUEUE);

	for (int i = 0; i < (int)num_queues; i++) {
		q_port_range[i].start = src_port_start + i * ports_per_queue;
		q_port_range[i].end   = q_port_range[i].start + ports_per_queue - 1;
		q_port_range[i].mask  = use_mask ? (0xFFFF ^ (ports_per_queue - 1)) : 0xFFFF;
		printf("Queue %d: src ports [%u, %u]%s\n",
		       i, q_port_range[i].start, q_port_range[i].end,
		       use_mask ? " (mask rule)" : " (per-port rules)");
	}
}

/*
 * Install one rte_flow rule per queue (or one per port in Option B) that
 * steers incoming responses to the queue that sent them.
 * The server echoes src<->dst, so responses arrive with dst_port == original
 * src_port. Matching on dst_port routes each response back to the right queue.
 *
 * Return 0 on success, -1 on a real validation/creation failure, or 1 if the
 * device doesn't implement rte_flow at all (e.g. virtio_user) -- in that
 * case no rules are installed and the caller falls back to running without
 * hardware queue steering.
 */
static int setup_flow_rules(uint8_t port)
{
	struct rte_flow_error error;
	struct rte_flow_attr attr = {
		.ingress  = 1,
		.priority = 0,  /* overrides RSS */
	};

	/* ETH and IPv4: use NULL spec/mask to match any (don't-care layers) */

	memset(flow_rules,      0, sizeof(flow_rules));
	memset(flow_rule_count, 0, sizeof(flow_rule_count));

	for (int q = 0; q < (int)num_queues; q++) {
		uint16_t port_lo   = q_port_range[q].start;
		uint16_t port_hi   = q_port_range[q].end;
		uint16_t port_mask = q_port_range[q].mask;

		struct rte_flow_action_queue queue_action = { .index = q };
		struct rte_flow_action actions[] = {
			{ RTE_FLOW_ACTION_TYPE_QUEUE, &queue_action },
			{ RTE_FLOW_ACTION_TYPE_END,   NULL          },
		};

		if (port_mask != 0xFFFF) {
			/* Option A: single mask-based rule covers the whole sub-range */
			struct rte_flow_item_udp udp_spec = {0}, udp_mask = {0};
			udp_spec.hdr.dst_port = rte_cpu_to_be_16(port_lo);
			udp_mask.hdr.dst_port = rte_cpu_to_be_16(port_mask);

			struct rte_flow_item pattern[] = {
				{ RTE_FLOW_ITEM_TYPE_ETH,  NULL, NULL, NULL },
				{ RTE_FLOW_ITEM_TYPE_IPV4, NULL, NULL, NULL },
				{ RTE_FLOW_ITEM_TYPE_UDP,  &udp_spec, NULL, &udp_mask },
				{ RTE_FLOW_ITEM_TYPE_END,  NULL, NULL, NULL },
			};

			int valid_ret = rte_flow_validate(port, &attr, pattern, actions, &error);
			if (valid_ret != 0) {
				if (valid_ret == -ENOTSUP || valid_ret == -ENOSYS) {
					printf("rte_flow not supported by this device; "
					       "skipping flow rule installation\n");
					return 1;
				}
				printf("Flow rule validation failed for queue %d (type=%d): %s\n",
				       q, error.type, error.message ? error.message : "(no message)");
				return -1;
			}
			flow_rules[q][0] = rte_flow_create(port, &attr, pattern, actions, &error);
			if (!flow_rules[q][0]) {
				printf("Flow rule creation failed for queue %d: %s\n",
				       q, error.message ? error.message : "(no message)");
				return -1;
			}
			flow_rule_count[q] = 1;
		} else {
			/* Option B: one rule per individual port in the sub-range */
			int rule_idx = 0;
			for (uint16_t p = port_lo; p <= port_hi; p++, rule_idx++) {
				struct rte_flow_item_udp udp_spec = {0}, udp_mask = {0};
				udp_spec.hdr.dst_port = rte_cpu_to_be_16(p);
				udp_mask.hdr.dst_port = rte_cpu_to_be_16(0xFFFF);

				struct rte_flow_item pattern[] = {
					{ RTE_FLOW_ITEM_TYPE_ETH,  NULL, NULL, NULL },
					{ RTE_FLOW_ITEM_TYPE_IPV4, NULL, NULL, NULL },
					{ RTE_FLOW_ITEM_TYPE_UDP,  &udp_spec, NULL, &udp_mask },
					{ RTE_FLOW_ITEM_TYPE_END,  NULL, NULL, NULL },
				};

				int valid_ret = rte_flow_validate(port, &attr, pattern, actions, &error);
				if (valid_ret != 0) {
					if (valid_ret == -ENOTSUP || valid_ret == -ENOSYS) {
						printf("rte_flow not supported by this device; "
						       "skipping flow rule installation\n");
						return 1;
					}
					printf("Flow rule validation failed for queue %d port %u (type=%d): %s\n",
					       q, p, error.type, error.message ? error.message : "(no message)");
					return -1;
				}
				flow_rules[q][rule_idx] = rte_flow_create(port, &attr, pattern, actions, &error);
				if (!flow_rules[q][rule_idx]) {
					printf("Flow rule creation failed for queue %d port %u: %s\n",
					       q, p, error.message ? error.message : "(no message)");
					return -1;
				}
			}
			flow_rule_count[q] = rule_idx;
		}

		printf("Installed %u flow rule(s) for queue %d\n", flow_rule_count[q], q);
	}
	return 0;
}

static void teardown_flow_rules(uint8_t port)
{
	struct rte_flow_error error;
	for (int q = 0; q < (int)num_queues; q++) {
		for (uint32_t r = 0; r < flow_rule_count[q]; r++) {
			if (flow_rules[q][r]) {
				rte_flow_destroy(port, flow_rules[q][r], &error);
				flow_rules[q][r] = NULL;
			}
		}
		flow_rule_count[q] = 0;
	}
}

/*
 * Run a dnetperf client
 */
static void do_client(uint8_t port)
{
	int flow_ret = setup_flow_rules(port);
	if (flow_ret < 0)
		rte_exit(EXIT_FAILURE, "Failed to install rte_flow rules\n");

	unsigned lcore_id = rte_lcore_id();
	struct open_loop_arg args[num_queues];
	
	// comment/uncomment if sendera and receiver are on the same thread 
	// 8<----------------------------------------------------------------
	// launch remote cores
	for (int i = 1; i < rte_lcore_count(); i++) {
		args[i].eth_addr = &server_eth;
		args[i].queue_id = i;
		lcore_id = rte_get_next_lcore(lcore_id, 1, 0);
		rte_eal_remote_launch(open_loop, &args[i], lcore_id);
	}
	
	// run on main core
	args[0].eth_addr = &server_eth;
	args[0].queue_id = 0;
	open_loop(&args[0]);
	// ------------------------------------------------------------------->8

	// NOTE: Not supported currently
	// comment/uncomment if sender and receiver are on different threads
	// 8<----------------------------------------------------------------
	/*for (int i = 0; i < (rte_lcore_count() - 1) / 2; i++) {*/
		/*args[i].eth_addr = &server_eth;*/
		/*args[i].queue_id = i;*/
		
		/*lcore_id = rte_get_next_lcore(lcore_id, 1, 0);*/
		/*rte_eal_remote_launch(open_loop_sender, &args[i], lcore_id);*/
		/*lcore_id = rte_get_next_lcore(lcore_id, 1, 0);*/
		/*rte_eal_remote_launch(open_loop_receiver, &args[i], lcore_id);*/
	/*}*/
	// ------------------------------------------------------------------->8
	
	rte_eal_mp_wait_lcore();

	teardown_flow_rules(port);

	uint64_t total_pkt_sent  = 0;
	uint64_t total_pkt_recv  = 0;
	uint64_t total_unique_rx = 0;
	uint64_t total_host_rx   = 0;
	uint64_t total_dpu_rx    = 0;

	uint64_t total_wrong_queue = 0;
	for (int i = 0; i < num_queues; i++) {
		total_pkt_sent  += port_statistics[i].tx;
		total_pkt_recv  += port_statistics[i].rx;
		total_unique_rx += port_statistics[i].unique_rx;
		total_host_rx   += port_statistics[i].host_rx;
		total_dpu_rx    += port_statistics[i].dpu_rx;
		total_wrong_queue += port_statistics[i].wrong_queue;
	}

	/* Per-queue queue-affinity report */
	printf("\nQueue affinity check (responses on wrong queue):\n");
	for (int i = 0; i < num_queues; i++) {
		printf("  Queue %d: rx=%lu  wrong_queue=%lu  (ports [%u,%u])\n",
		       i, port_statistics[i].rx, port_statistics[i].wrong_queue,
		       q_port_range[i].start, q_port_range[i].end);
	}
	printf("  Total wrong-queue responses: %lu / %lu\n\n", total_wrong_queue, total_pkt_recv);

	if (seconds != 0) {
		printf("Sent: %lu\tReceived: %lu\tMissing: %lu\n", total_pkt_sent, total_unique_rx, total_pkt_sent - total_unique_rx);
		printf("Sent: %f Mpps\tReceived: %f Mpps\tMissing: %f Mpps\n",
			(double) total_pkt_sent  / (send_end_time - send_start_time) * rte_get_timer_hz() / 1000000,
			(double) total_unique_rx / (send_end_time - send_start_time) * rte_get_timer_hz() / 1000000,
			(double) (total_pkt_sent - total_unique_rx) / (send_end_time - send_start_time) * rte_get_timer_hz() / 1000000);
		printf("Host: %f Mpps\tDPU: %f Mpps\n",
			(double) total_host_rx / (send_end_time - send_start_time) * rte_get_timer_hz() / 1000000,
			(double) total_dpu_rx  / (send_end_time - send_start_time) * rte_get_timer_hz() / 1000000);
		printf("Total Received: %lu\n", total_pkt_recv);
		report_stats();
	}
	
	if (output_filename != NULL) {
		dump_stats();
	}
}

/*
 * Initialize dpdk.
 */
static int dpdk_init(int argc, char *argv[])
{
	int args_parsed;
	int nb_ports = 1;
	int nb_rxd = RX_RING_SIZE;
	int nb_txd = TX_RING_SIZE;
	int nb_lcores;
	unsigned int nb_mbufs;

	/* Initialize the Environment Abstraction Layer (EAL). */
	args_parsed = rte_eal_init(argc, argv);
	if (args_parsed < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	nb_lcores = rte_lcore_count();

	/* Check that there is a port to send/receive on. */
	if (!rte_eth_dev_is_valid_port(0))
		rte_exit(EXIT_FAILURE, "Error: no available ports\n");

	nb_mbufs = RTE_MAX(nb_lcores * (nb_rxd + nb_txd + MEMPOOL_CACHE_SIZE) +
		MAX_PKT_BURST, 8192U);

	/* Creates a new mempool in memory to hold the mbufs. */
	rx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_RX_POOL", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (rx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create rx mbuf pool\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	tx_mbuf_pool = rte_pktmbuf_pool_create("MBUF_TX_POOL", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (tx_mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create tx mbuf pool\n");

	return args_parsed;
}

static void print_long_usage(const char *prog)
{
	printf(
"Usage: %s [EAL args] -- [OPTIONS]\n"
"\n"
"  -h            Print short usage summary and exit.\n"
"  --help        Print this message and exit.\n"
"\n"
"REQUIRED OPTIONS\n"
"  -s, --server <ip>\n"
"        IPv4 address of the server (e.g. 192.168.1.1).\n"
"\n"
"  -M, --server-mac <mac>\n"
"        Ethernet MAC address of the server in colon-separated hex\n"
"        (e.g. aa:bb:cc:dd:ee:ff).  Used to fill the destination MAC of\n"
"        every outgoing packet without an ARP lookup.\n"
"\n"
"  -p, --port <port>\n"
"        Destination UDP port on the server (e.g. 10000).\n"
"\n"
"  -t, --time <seconds>\n"
"        Experiment duration in seconds.  Use 0 for unlimited / throughput\n"
"        mode (gen_unlimited).  In unlimited mode retry tracking, latency\n"
"        collection, and the pending table are all disabled so the TX path\n"
"        has zero extra overhead.\n"
"\n"
"      --warmup <seconds>\n"
"        Seconds at the start of the experiment during which latency samples\n"
"        are discarded.  Allows the server to reach steady state before\n"
"        statistics are recorded.  Default: 2.\n"
"\n"
"  -T, --type <type>\n"
"        Payload type.  One of:\n"
"          get    DMA read request (get_set_entry_t, op_type=0)\n"
"          set    DMA write request (get_set_entry_t + DMA_RW_SIZE bytes)\n"
"          list   Linked-list traversal (4-node walk)\n"
"          ht     Hash-table GET/SET mix (YCSB workload; see --workload)\n"
"          bpt    B+ tree lookup (bpt_entry_t, fixed key=6)\n"
"\n"
"  -b, --batch <n>\n"
"        Number of packets to allocate and transmit per TX burst.  Also\n"
"        controls the nseq batch-FAA size (one atomic per burst).\n"
"\n"
"  -n, --funcs <n>\n"
"        Number of server-side functions.  func_id is chosen uniformly at\n"
"        random in [0, n) per packet.\n"
"\n"
"  -m, --loadgen <type>\n"
"        Load-generator mode.  One of:\n"
"          fixed      Constant rate at --pps for the full run.\n"
"          variable   Ramp from --pps up to --pps-end in steps of\n"
"                     --pps-step every --pps-time seconds, then hold.\n"
"          ladder     Same as variable but ramps back down to --pps\n"
"                     after reaching --pps-end.\n"
"\n"
"  -r, --pps <n>\n"
"        Offered load in packets per second across all queues.  In fixed\n"
"        mode this is the only rate used.  In variable/ladder mode this is\n"
"        the starting rate.\n"
"\n"
"VARIABLE / LADDER MODE OPTIONS  (all three required when -m variable|ladder)\n"
"  -e, --pps-end <n>\n"
"        Target PPS ceiling for variable/ladder ramps.\n"
"\n"
"  -k, --pps-step <n>\n"
"        PPS increment applied every --pps-time seconds.\n"
"\n"
"  -i, --pps-time <seconds>\n"
"        Interval in seconds between each PPS step.\n"
"\n"
"OPTIONAL OPTIONS\n"
"  -o, --output <file>\n"
"        Write per-packet CSV stats to <file>.\n"
"\n"
"      --src-port-start <port>   (default: %u)\n"
"        First UDP source port in the client's port range.  The range\n"
"        [src-port-start, src-port-end] is divided evenly across queues.\n"
"        Each queue owns a contiguous sub-range and rte_flow rules steer\n"
"        responses back to the correct queue.\n"
"\n"
"      --src-port-end <port>     (default: %u)\n"
"        Last UDP source port, inclusive.  The total range width must be\n"
"        divisible by the number of queues.  For Option-A rte_flow rules\n"
"        (single mask per queue) the width per queue must also be a power\n"
"        of two and src-port-start must be aligned to that width; otherwise\n"
"        one rte_flow rule per individual port is created (Option B).\n"
"\n"
"      --retry-timeout <us>      (default: %lu)\n"
"        Per-packet retry timeout in microseconds.  If no response is\n"
"        received within this window the packet is retransmitted (up to\n"
"        --max-retries times).  The timeout is measured from the most\n"
"        recent send time; RTT is always measured from the first send.\n"
"        Ignored in gen_unlimited mode (-t 0).\n"
"\n"
"      --max-retries <n>         (default: %u)\n"
"        Maximum number of retransmit attempts per packet.  After this\n"
"        limit the slot is freed and the request is counted as lost in\n"
"        \"Retry exhausted\" stats.  0 disables retries entirely.\n"
"\n"
"      --pending-scale <n>       (default: %u)\n"
"        Headroom multiplier for the per-queue pending table.\n"
"        max_pending = next_pow2(max_in_flight * pending-scale), where\n"
"        max_in_flight = retry-timeout * pps_per_queue / 1e6.  Larger\n"
"        values reduce wrap-around collisions at the cost of more memory.\n"
"        A warning is printed if the resulting table exceeds 65536 slots.\n"
"\n"
"      --num-keys <n>            (default: %lu, from compiled-in KEY_NUM_BITS)\n"
"        Total number of keys in the server hashtable.  The client derives\n"
"        a key mask as next_pow2(n)-1 so generated keys fall in [0, n).\n"
"        Must match the --num-keys value used when starting the server.\n"
"\n"
"HT WORKLOAD OPTIONS  (only meaningful with -T ht)\n"
"      --workload <A|B|C>        (default: B)\n"
"        YCSB workload read/write mix:\n"
"          A   50%% GET / 50%% SET\n"
"          B   95%% GET /  5%% SET  (default, matches prior behaviour)\n"
"          C  100%% GET /  0%% SET\n"
"\n"
"      --key-dist <uniform|zipf> (default: uniform)\n"
"        Key selection distribution for HT workloads.\n"
"          uniform  Keys drawn uniformly at random over [0, num-keys).\n"
"          zipf     Keys drawn from a Zipf distribution (see --zipf-theta).\n"
"\n"
"      --zipf-theta <theta>      (default: 0.99)\n"
"        Zipf skew exponent in (0, 1).  Higher values concentrate more\n"
"        traffic on the hottest keys (YCSB default is 0.99).\n"
"        Only used when --key-dist zipf is set.\n"
"\n"
"      --warmup <seconds>        (default: 2)\n"
"        Seconds at the start of the run during which latency samples are\n"
"        discarded.  Allows the server to reach steady state before\n"
"        statistics are recorded.  Use 0 to disable.\n"
"\n"
"EXAMPLES\n"
"  # Fixed rate, hash-table workload, 2 queues, 3 Mpps for 10 s:\n"
"  client [EAL] -- -s 10.0.0.1 -M aa:bb:cc:dd:ee:ff -p 10000 \\\n"
"      -t 10 -T ht -b 32 -n 4 -m fixed -r 3000000 \\\n"
"      --src-port-start 1024 --src-port-end 1031\n"
"\n"
"  # Ladder ramp 1→10 Mpps in 1 M steps, retries enabled:\n"
"  client [EAL] -- -s 10.0.0.1 -M aa:bb:cc:dd:ee:ff -p 10000 \\\n"
"      -t 30 -T ht -b 32 -n 4 -m ladder \\\n"
"      -r 1000000 -e 10000000 -k 1000000 -i 2 \\\n"
"      --src-port-start 1024 --src-port-end 1031 \\\n"
"      --retry-timeout 500 --max-retries 3\n"
"\n"
"  # Unlimited throughput test (no latency/retry overhead):\n"
"  client [EAL] -- -s 10.0.0.1 -M aa:bb:cc:dd:ee:ff -p 10000 \\\n"
"      -t 0 -T get -b 32 -n 1 -m fixed -r 1\n"
"\n",
		prog,
		SRC_PORT_START, SRC_PORT_MAX,
		retry_timeout_us, (unsigned)max_retries, pending_scale,
		(uint64_t)(key_mask + 1));
}

/*
 * Application arguments (passed after EAL args and --).
 *
 * Required:
 *   -s, --server <ip>          Server IP address
 *   -M, --server-mac <mac>     Server MAC address (xx:xx:xx:xx:xx:xx)
 *   -p, --port <port>          Server UDP port
 *   -t, --time <seconds>       Run duration (0 = unlimited)
 *   -T, --type <type>          Payload type: get | set | list | ht | bpt
 *   -b, --batch <n>            TX batch size
 *   -n, --funcs <n>            Number of server functions
 *   -m, --loadgen <type>       Load generator: fixed | variable | ladder
 *   -r, --pps <n>              Offered packets per second (start value)
 *
 * Variable / ladder mode (all three required):
 *   -e, --pps-end <n>          PPS end value
 *   -k, --pps-step <n>         PPS step per interval
 *   -i, --pps-time <seconds>   Seconds per step
 *
 * Optional:
 *   -o, --output <file>        Per-packet stats output file
 *       --src-port-start <p>   First source port (default: SRC_PORT_START)
 *       --src-port-end <p>     Last source port, inclusive (default: SRC_PORT_MAX)
 *       --retry-timeout <us>   Retry timeout in microseconds (default: 1000)
 *       --max-retries <n>      Maximum retry attempts (default: 3)
 *       --pending-scale <n>    Pending table scale factor (default: 2)
 *   -h                         Print short usage and exit
 *       --help                 Print long usage and exit
 */
static int parse_netperf_args(int argc, char *argv[])
{
	static struct option long_opts[] = {
		{ "server",         required_argument, NULL, 's' },
		{ "server-mac",     required_argument, NULL, 'M' },
		{ "port",           required_argument, NULL, 'p' },
		{ "time",           required_argument, NULL, 't' },
		{ "type",           required_argument, NULL, 'T' },
		{ "batch",          required_argument, NULL, 'b' },
		{ "funcs",          required_argument, NULL, 'n' },
		{ "loadgen",        required_argument, NULL, 'm' },
		{ "pps",            required_argument, NULL, 'r' },
		{ "pps-end",        required_argument, NULL, 'e' },
		{ "pps-step",       required_argument, NULL, 'k' },
		{ "pps-time",       required_argument, NULL, 'i' },
		{ "output",         required_argument, NULL, 'o' },
		{ "src-port-start", required_argument, NULL,  1  },
		{ "src-port-end",   required_argument, NULL,  2  },
		{ "retry-timeout",  required_argument, NULL,  3  },
		{ "max-retries",    required_argument, NULL,  4  },
		{ "pending-scale",  required_argument, NULL,  5  },
		{ "help",           no_argument,       NULL,  6  }, /* long help */
		{ "num-keys",       required_argument, NULL,  7  },
		{ "workload",       required_argument, NULL,  8  },
		{ "key-dist",       required_argument, NULL,  9  },
		{ "zipf-theta",     required_argument, NULL, 10  },
		{ "warmup",         required_argument, NULL, 11  },
		{ NULL, 0, NULL, 0 }
	};

	bool have_server = false, have_mac   = false, have_port  = false;
	bool have_time   = false, have_type  = false, have_batch  = false;
	bool have_funcs  = false, have_loadgen = false, have_pps  = false;
	int opt, longidx;

	while ((opt = getopt_long(argc, argv, "s:M:p:t:T:b:n:m:r:e:k:i:o:h",
	                          long_opts, &longidx)) != -1) {
		switch (opt) {
		case 's':
			str_to_ip(optarg, &server_ip);
			have_server = true;
			break;
		case 'M':
			if (rte_ether_unformat_addr(optarg, &server_eth) != 0) {
				fprintf(stderr, "Invalid MAC address '%s'\n", optarg);
				return -EINVAL;
			}
			have_mac = true;
			break;
		case 'p':
			server_port = (unsigned int)atoi(optarg);
			have_port = true;
			break;
		case 't':
			seconds = (int)atol(optarg);
			have_time = true;
			break;
		case 'T':
			payload_len = offsetof(req_pkt_t, data) - offsetof(req_pkt_t, pad0);
			if (!strcmp(optarg, "get")) {
				payload_type = PAYLOAD_TYPE_GET;
				payload_len += sizeof(get_set_entry_t);
			} else if (!strcmp(optarg, "set")) {
				payload_type = PAYLOAD_TYPE_SET;
				payload_len += sizeof(get_set_entry_t) + DMA_RW_SIZE;
				dma_data = (char *)malloc(DMA_RW_SIZE);
				memset(dma_data, 'a', DMA_RW_SIZE);
			} else if (!strcmp(optarg, "list")) {
				payload_type = PAYLOAD_TYPE_LIST;
				payload_len += BUF_SIZE_LLIST;
			} else if (!strcmp(optarg, "ht")) {
				payload_type = PAYLOAD_TYPE_HT;
				payload_len += sizeof(ht_entry_t);
			} else if (!strcmp(optarg, "bpt")) {
				payload_type = PAYLOAD_TYPE_BPT;
				payload_len += sizeof(bpt_entry_t);
			} else {
				fprintf(stderr, "Invalid payload type '%s'\n", optarg);
				return -EINVAL;
			}
			have_type = true;
			break;
		case 'b':
			send_batch_size = (unsigned int)atoi(optarg);
			recv_batch_size = recv_batch_size > send_batch_size
			                  ? recv_batch_size : send_batch_size;
			have_batch = true;
			break;
		case 'n':
			num_funcs = (uint8_t)atoi(optarg);
			have_funcs = true;
			break;
		case 'm':
			if (!strcmp(optarg, "fixed"))
				loadgen_type = LOADGEN_TYPE_FIXED;
			else if (!strcmp(optarg, "variable"))
				loadgen_type = LOADGEN_TYPE_VARIABLE;
			else if (!strcmp(optarg, "ladder"))
				loadgen_type = LOADGEN_TYPE_LADDER;
			else {
				fprintf(stderr, "Invalid loadgen type '%s'\n", optarg);
				return -EINVAL;
			}
			have_loadgen = true;
			break;
		case 'r':
			send_pps_start = atol(optarg);
			have_pps = true;
			break;
		case 'e':
			send_pps_end = atol(optarg);
			break;
		case 'k':
			send_pps_step = atol(optarg);
			break;
		case 'i':
			send_pps_trigger_time = (int)atol(optarg);
			break;
		case 'o':
			output_filename = optarg;
			break;
		case 1:
			src_port_start = (uint16_t)atoi(optarg);
			break;
		case 2:
			src_port_end = (uint16_t)atoi(optarg);
			break;
		case 3:
			retry_timeout_us = (uint64_t)atol(optarg);
			break;
		case 4:
			max_retries = (uint8_t)atoi(optarg);
			break;
		case 5:
			pending_scale = (uint32_t)atoi(optarg);
			break;
		case 6: /* --help: long usage */
			print_long_usage(argv[0]);
			exit(0);
		case 7: { /* --num-keys */
			uint64_t nk = (uint64_t)atol(optarg);
			if (nk == 0) {
				fprintf(stderr, "--num-keys must be > 0\n");
				return -EINVAL;
			}
			/* key_mask = next power-of-2 >= nk, minus 1 */
			uint64_t m = nk - 1;
			m |= m >> 1; m |= m >> 2; m |= m >> 4;
			m |= m >> 8; m |= m >> 16; m |= m >> 32;
			key_mask = m;
			break;
		}
		case 8: { /* --workload */
			if (!strcmp(optarg, "A") || !strcmp(optarg, "a"))
				ycsb_workload = 0;
			else if (!strcmp(optarg, "B") || !strcmp(optarg, "b"))
				ycsb_workload = 1;
			else if (!strcmp(optarg, "C") || !strcmp(optarg, "c"))
				ycsb_workload = 2;
			else {
				fprintf(stderr, "--workload must be A, B, or C\n");
				return -EINVAL;
			}
			break;
		}
		case 9: /* --key-dist */
			if (!strcmp(optarg, "zipf"))
				use_zipf = true;
			else if (!strcmp(optarg, "uniform"))
				use_zipf = false;
			else {
				fprintf(stderr, "--key-dist must be 'uniform' or 'zipf'\n");
				return -EINVAL;
			}
			break;
		case 10: /* --zipf-theta */
			zipf_theta = atof(optarg);
			if (zipf_theta <= 0.0 || zipf_theta >= 1.0) {
				fprintf(stderr, "--zipf-theta must be in (0, 1)\n");
				return -EINVAL;
			}
			break;
		case 11: /* --warmup */
			warmup_seconds = atoi(optarg);
			if (warmup_seconds < 0) {
				fprintf(stderr, "--warmup must be >= 0\n");
				return -EINVAL;
			}
			break;
		case 'h':
			fprintf(stderr,
			    "Usage: %s [EAL args] -- "
			    "-s <ip> -M <mac> -p <port> -t <sec> -T <get|set|list|ht|bpt> "
			    "-b <n> -n <n> -m <fixed|variable|ladder> -r <pps> "
			    "[-e <pps-end> -k <pps-step> -i <pps-time>] "
			    "[-o <file>] [--src-port-start <p>] [--src-port-end <p>] "
			    "[--retry-timeout <us>] [--max-retries <n>] [--pending-scale <n>] "
			    "[--num-keys <n>] [--workload <A|B|C>] "
			    "[--key-dist <uniform|zipf>] [--zipf-theta <theta>]\n"
			    "Use --help for full documentation.\n",
			    argv[0]);
			exit(0);
		default:
			fprintf(stderr,
			    "Unknown option. "
			    "Use -h for short usage or --help for full documentation.\n");
			return -EINVAL;
		}
	}

	if (!have_server || !have_mac || !have_port || !have_time || !have_type ||
	    !have_batch || !have_funcs || !have_loadgen || !have_pps) {
		fprintf(stderr, "Missing required argument(s). Use -h for help.\n");
		return -EINVAL;
	}

	if ((loadgen_type == LOADGEN_TYPE_VARIABLE || loadgen_type == LOADGEN_TYPE_LADDER) &&
	    (!send_pps_end || !send_pps_step || !send_pps_trigger_time)) {
		fprintf(stderr, "variable/ladder mode requires --pps-end, --pps-step, --pps-time\n");
		return -EINVAL;
	}

	if (DEBUG_ARGS) {
		printf("Server IP:   "); print_ip_u32(server_ip); printf("\n");
		printf("Server MAC:  %02x:%02x:%02x:%02x:%02x:%02x\n",
		       server_eth.addr_bytes[0], server_eth.addr_bytes[1],
		       server_eth.addr_bytes[2], server_eth.addr_bytes[3],
		       server_eth.addr_bytes[4], server_eth.addr_bytes[5]);
		printf("Server port: %u\n", server_port);
		printf("Run time:    %d\n", seconds);
		printf("Warmup:      %d s\n", warmup_seconds);
		printf("Batch size:  %u\n", send_batch_size);
		printf("Num funcs:   %d\n", num_funcs);
		if (loadgen_type == LOADGEN_TYPE_VARIABLE || loadgen_type == LOADGEN_TYPE_LADDER) {
			printf("PPS start:   %lu\n", send_pps_start);
			printf("PPS end:     %lu\n", send_pps_end);
			printf("PPS step:    %lu\n", send_pps_step);
			printf("PPS time:    %d\n",  send_pps_trigger_time);
		} else {
			printf("PPS:         %lu\n", send_pps_start);
		}
		if (output_filename)
			printf("Output:      %s\n", output_filename);
		printf("Src ports:   %u – %u\n", src_port_start, src_port_end);
		printf("Retry timeout: %lu us\n", retry_timeout_us);
		printf("Max retries:   %u\n", max_retries);
		printf("Pending scale: %u\n", pending_scale);
		printf("Key mask:      0x%lx (num_keys <= %lu)\n", key_mask, key_mask + 1);
		if (payload_type == PAYLOAD_TYPE_HT) {
			printf("HT workload:   YCSB-%c %s\n",
			       'A' + ycsb_workload, ycsb_name[ycsb_workload]);
			printf("Key dist:      %s\n", use_zipf ? "zipf" : "uniform");
			if (use_zipf)
				printf("Zipf theta:    %.4f\n", zipf_theta);
		}
	}

	return 0;
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	int args_parsed, res, lcore_id;
	uint64_t i;

	/*rte_atomic64_init(&num_get_req);*/
	/*rte_atomic64_init(&num_set_req);*/

	/* Initialize dpdk. */
	args_parsed = dpdk_init(argc, argv);

	/* initialize our arguments */
	argc -= args_parsed;
	argv += args_parsed;
	res = parse_netperf_args(argc, argv);
	if (res < 0)
		return 0;

	/* initialize port */
	/* NOTE: Not supported currently */
	// Uncomment if sender and receiver on different thread
	// open_loop_sender() && open_loop_receiver()
	/*num_queues = (rte_lcore_count() - 1) / 2;*/

	// Uncomment if sender and receiver on same thread (open_loop())
	num_queues = rte_lcore_count();
	
	if (port_init(dpdk_port, rx_mbuf_pool, num_queues) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n", dpdk_port);

	compute_queue_port_ranges();

	/* Compute retry timing and allocate pending tables */
	retry_timeout_cycles = retry_timeout_us * rte_get_timer_hz() / 1000000;
	{
		uint64_t pps_per_queue = ((send_pps_end > 0 ? send_pps_end : send_pps_start)
		                          / num_queues) + 1;
		uint64_t max_in_flight = (retry_timeout_us * pps_per_queue) / 1000000 + 1;
		if (max_in_flight < send_batch_size)
			max_in_flight = send_batch_size;
		max_pending = next_pow2(max_in_flight * pending_scale);
		if (max_pending < 64)
			max_pending = 64;
		if (max_pending > 65536)
			printf("WARNING: max_pending=%u is large and may not fit in L2 cache\n",
			       max_pending);
		printf("Retry: timeout=%lu us  max_retries=%u  max_pending=%u\n",
		       retry_timeout_us, max_retries, max_pending);
		for (unsigned int qi = 0; qi < num_queues; qi++) {
			pending[qi].slots = calloc(max_pending, sizeof(struct pending_req));
			pending[qi].fifo.ring = calloc(max_pending, sizeof(uint64_t));
			if (!pending[qi].slots || !pending[qi].fifo.ring)
				rte_exit(EXIT_FAILURE, "Cannot allocate pending table for queue %u\n", qi);
		}
	}

	/* Initialize sample arrays */
	diff_times = (uint64_t *)malloc(MAX_SAMPLES * sizeof(uint64_t));
	if (!diff_times)
		rte_exit(EXIT_FAILURE, "Cannot allocate memory for diff_times\n");

	// Initialize per queue data structures
	for (i = 0; i < num_queues; i++) {
		rtt_times[i].data = calloc(MAX_SAMPLES / num_queues, sizeof(struct pkt_sample));
		if (!rtt_times[i].data)
			rte_exit(EXIT_FAILURE, "Cannot allocate memory for rtt_times\n");
		
		rand_state[i].a = rte_get_timer_cycles();
	}

	if (payload_type == PAYLOAD_TYPE_HT && use_zipf)
		zipf_init(&g_zipf, key_mask + 1, zipf_theta);

	do_client(dpdk_port);

	return 0;
}
