/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_bus_pci.h>
#include <rte_dev.h>
#include <rte_bus.h>

static volatile bool force_quit;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/* Port configuration */
static unsigned int port_id = 2;

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 1024
#define RTE_TEST_TX_DESC_DEFAULT 1024
#define RTE_MAX_TX_RX_QUEUE 16
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static unsigned int nb_queues = 1;

static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_TX_RX_QUEUE];

static struct rte_eth_conf default_port_conf = {
	.rxmode = {
		.mq_mode = RTE_ETH_MQ_RX_RSS,
		.max_lro_pkt_size = RTE_ETHER_MAX_LEN,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP
		},
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
	},
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_TX_RX_QUEUE];

uint64_t old_tx = 0;
uint64_t old_rx = 0;
uint64_t old_miss = 0;

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 10; /* default period is 10 seconds */

/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	unsigned queueid;
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	struct rte_eth_stats stats;
	uint64_t new_tx, new_rx, new_miss;

	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);
	
	printf("\nPort statistics ====================================");

	for (queueid = 0; queueid < nb_queues; queueid++) {
		/* skip disabled ports */
		printf("\nStatistics for queue %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64,
			   queueid,
			   port_statistics[queueid].tx,
			   port_statistics[queueid].rx);
	}

	rte_eth_stats_get(port_id, &stats);

	new_tx = stats.opackets - old_tx;
	old_tx = stats.opackets;

	new_rx = stats.ipackets - old_rx;
	old_rx = stats.ipackets;

	new_miss = stats.imissed - old_miss;
	old_miss = stats.imissed;

	printf("\nAggregate statistics for last %d seconds ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   timer_period / rte_get_timer_hz(),
		   new_tx,
		   new_rx,
		   new_miss);
	printf("\n========================================================================\n");


	fflush(stdout);
}

static void
handle_packet(struct rte_mbuf *m, unsigned queue_id)
{
	struct rte_ether_hdr *ptr_mac_hdr;
	struct rte_ether_addr src_addr;
	struct rte_ipv4_hdr *ptr_ipv4_hdr;
	uint32_t src_ip_addr;
	uint16_t tmp_port;
	int sent;
	struct rte_eth_dev_tx_buffer *buffer;
	char *mem;

	/* swap src and dst ether addresses */
	ptr_mac_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	rte_ether_addr_copy(&ptr_mac_hdr->src_addr, &src_addr);
	rte_ether_addr_copy(&ptr_mac_hdr->dst_addr, &ptr_mac_hdr->src_addr);
	rte_ether_addr_copy(&src_addr, &ptr_mac_hdr->dst_addr);

	/* swap src and dst IP addresses */
	ptr_ipv4_hdr = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr *,
					RTE_ETHER_HDR_LEN);
	src_ip_addr = ptr_ipv4_hdr->src_addr;
	ptr_ipv4_hdr->src_addr = ptr_ipv4_hdr->dst_addr;
	ptr_ipv4_hdr->dst_addr = src_ip_addr;

	/* swap UDP ports */
	struct rte_udp_hdr *rte_udp_hdr;
	rte_udp_hdr = rte_pktmbuf_mtod_offset(m, struct rte_udp_hdr *,
					RTE_ETHER_HDR_LEN + sizeof(struct rte_ipv4_hdr));
	tmp_port = rte_udp_hdr->src_port;
	rte_udp_hdr->src_port = rte_udp_hdr->dst_port;
	rte_udp_hdr->dst_port = tmp_port;

	buffer = tx_buffer[queue_id];
	sent = rte_eth_tx_buffer(port_id, queue_id, buffer, m);
	if (sent)
		port_statistics[queue_id].tx += sent;
}

/* main processing loop */
static int
l2fwd_main_loop(void *arg)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	uint64_t queue_id = (uint64_t)arg;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {
			sent = rte_eth_tx_buffer_flush(port_id, queue_id, tx_buffer[queue_id]);
			if (sent)
				port_statistics[queue_id].tx += sent;

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on main core */
					if (lcore_id == rte_get_main_lcore()) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */

		nb_rx = rte_eth_rx_burst(port_id, queue_id,
					 pkts_burst, MAX_PKT_BURST);

		port_statistics[queue_id].rx += nb_rx;

		for (j = 0; j < nb_rx; j++) {
			m = pkts_burst[j];
			rte_prefetch0(rte_pktmbuf_mtod(m, void *));
			handle_packet(m, queue_id);
		}
	}

	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- [-T PERIODI]\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  -q NQ: number of queues (should match with number of lcores, default is 1)\n",
	       prgname);
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= RTE_MAX_TX_RX_QUEUE)
		return 0;
	
	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
	"T:"  /* timer period */
	"q:"  /* number of total queues */
	;

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, short_options,
				  NULL, &option_index)) != EOF) {

		switch (opt) {
		/* timer period */
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			timer_period = timer_secs;
			break;
		
		/* nqueue */
		case 'q':
			nb_queues = l2fwd_parse_nqueue(optarg);
			if (nb_queues == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}

			if (nb_queues != rte_lcore_count()) {
				printf("number of queues %d doesn't match with number of lcores %d\n",
						nb_queues, rte_lcore_count());
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t port_id;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(port_id) {
			if (force_quit)
				return;
			if ((port_mask & (1 << port_id)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(port_id, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						port_id, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
					sizeof(link_status_text), &link);
				printf("Port %d %s\n", port_id,
				       link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	int ret, i;
	uint16_t nb_ports;
	unsigned lcore_id;
	unsigned int nb_lcores = rte_lcore_count();
	unsigned int nb_mbufs;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();
    
	nb_ports = rte_eth_dev_count_avail();
    if (nb_ports > RTE_MAX_ETHPORTS)
        nb_ports = RTE_MAX_ETHPORTS;

    printf(" %-4s %-12s %-6s %-12s %-5s %s\n", "Port:", "Name", "IfIndex", "Alias", "NUMA", "PCI");
    for (i = 0; i < nb_ports; i++) {
        struct rte_eth_dev_info dev;
        char buff[64];

        rte_eth_dev_info_get(i, &dev);

        buff[0] = 0;
        printf("   %2d: %-12s   %2d    %-12s  %2d   ", i, dev.driver_name, dev.if_index,
               rte_driver_name(rte_dev_driver(dev.device)), rte_dev_numa_node(dev.device));
        {
            struct rte_bus *bus;
            if (dev.device)
                bus = rte_bus_find_by_device(dev.device);
            else
                bus = NULL;
            if (bus && !strcmp(rte_bus_name(bus), "pci")) {
                snprintf(buff, sizeof(buff), "%s", rte_dev_name(dev.device));
            }
        }
        printf("%s\n", buff);
    }
    printf("\n");

	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* Initialise each port */
	struct rte_eth_conf port_conf;
	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;
	struct rte_eth_dev_info dev_info;
	struct rte_ether_addr addr;

	/* init port */
	printf("Initializing port %u... ", port_id);
	fflush(stdout);



	ret = rte_eth_dev_info_get(port_id, &dev_info);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			"Error during getting device (port %u) info: %s\n",
			port_id, strerror(-ret));
	
	/* Get a clean copy of the configuration structure */
	rte_memcpy(&port_conf, &default_port_conf, sizeof(struct rte_eth_conf));

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
	if (nb_queues > 1) {
		port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
		port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
	} else {
		port_conf.rx_adv_conf.rss_conf.rss_key = NULL;
		port_conf.rx_adv_conf.rss_conf.rss_hf  = 0;
	}

	if (port_conf.rx_adv_conf.rss_conf.rss_hf != 0)
		port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
	else
		port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

	ret = rte_eth_dev_configure(port_id, nb_queues, nb_queues, &port_conf);
	
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
			  ret, port_id);

	ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd,
						   &nb_txd);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			 "Cannot adjust number of descriptors: err=%d, port=%u\n",
			 ret, port_id);

	ret = rte_eth_macaddr_get(port_id, &addr);
	if (ret < 0)
		rte_exit(EXIT_FAILURE,
			 "Cannot get MAC address: err=%d, port=%u\n",
			 ret, port_id);

	/* init one RX queue */
	fflush(stdout);
	for (int i = 0; i < nb_queues; i++) {
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(port_id, i, nb_rxd,
						 rte_eth_dev_socket_id(port_id),
						 &rxq_conf,
						 l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, port_id);

		/* initialize TX queues */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(port_id, i, nb_txd,
				rte_eth_dev_socket_id(port_id),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, port_id);

		/* Initialize TX buffers */
		tx_buffer[i] = rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(port_id));
		
		if (tx_buffer[i] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					port_id);

		rte_eth_tx_buffer_init(tx_buffer[i], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[i],
				rte_eth_tx_buffer_count_callback,
				&port_statistics[i].dropped);

		if (ret < 0)
			rte_exit(EXIT_FAILURE,
			"Cannot set error callback for tx buffer on port %u\n",
				 port_id);
	}

	ret = rte_eth_dev_set_ptypes(port_id, RTE_PTYPE_UNKNOWN, NULL,
					 0);

	if (ret < 0)
		printf("Port %u, Failed to disable Ptype parsing\n",
				port_id);
	
	/* Start device */
	ret = rte_eth_dev_start(port_id);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
			  ret, port_id);

	printf("done: \n");

	ret = rte_eth_promiscuous_enable(port_id);
	if (ret != 0)
		rte_exit(EXIT_FAILURE,
			 "rte_eth_promiscuous_enable:err=%s, port=%u\n",
			 rte_strerror(-ret), port_id);

	printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
			port_id,
			addr.addr_bytes[0],
			addr.addr_bytes[1],
			addr.addr_bytes[2],
			addr.addr_bytes[3],
			addr.addr_bytes[4],
			addr.addr_bytes[5]);

	/* initialize port stats */
	memset(&port_statistics, 0, sizeof(port_statistics));

	ret = 0;
	int q = 0;
	/* launch per-lcore init on every lcore */
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		rte_eal_remote_launch(l2fwd_main_loop, (void *)q, lcore_id);
		q++;
	}

	l2fwd_main_loop((void *)q);

	rte_eal_mp_wait_lcore();
		
	printf("Closing port %d...", port_id);
	ret = rte_eth_dev_stop(port_id);
	if (ret != 0)
		printf("rte_eth_dev_stop: err=%d, port=%d\n",
			   ret, port_id);
	rte_eth_dev_close(port_id);
	printf(" Done\n");
	printf("Bye...\n");
	
	return ret;
}
