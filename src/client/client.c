#include <arpa/inet.h>
#include <assert.h>
#include <generic/rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_build_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_udp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define TX_RING_SIZE 1024
#define RX_RING_SIZE 1024
#define RX_QUEUES_PER_PORT 2
#define MAX_BURST_SIZE 32

struct mbuf_table {
  uint32_t len;
  struct rte_mbuf *m_table[MAX_BURST_SIZE];
};

struct lcore_configuration {
  uint32_t vid;
  uint32_t port;
  uint32_t tx_queue_id;
  struct mbuf_table tx_mbufs;
} __rte_cache_aligned;

struct throughput_statistics {
  uint64_t tx_bits;
  uint64_t last_tx_bits;
  uint64_t dropped_pkts;
  uint64_t last_dropped_pkts;
} __rte_cache_aligned;

struct MessageHeader {
  uint32_t seq_num;
  uint32_t rank;

  uint8_t fill_pkt[1450];
} __rte_packed;

struct lcore_configuration lcore_conf[RTE_MAX_LCORE];
struct throughput_statistics tput_stat[RTE_MAX_LCORE];
uint8_t header_template[sizeof(struct rte_ether_hdr) +
                        sizeof(struct rte_ipv4_hdr) +
                        sizeof(struct rte_udp_hdr)];
uint64_t pkts_send_limit_per_ms = 800;
uint64_t time_to_run = 10;

uint32_t n_lcores;

struct rte_mempool *pktmbuf_pool;

void init_header_template() {
  memset(header_template, 0, sizeof(header_template));

  struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)header_template;
  struct rte_ipv4_hdr *ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
  struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ipv4_hdr + 1);

  uint32_t pkt_len = sizeof(header_template) + sizeof(struct MessageHeader);

  // ethernet header
  eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  struct rte_ether_addr src_addr = {
      .addr_bytes = {0x00, 0x01, 0x02, 0x00, 0x00, 0x01}};
  struct rte_ether_addr dst_addr = {
      .addr_bytes = {0x00, 0x01, 0x02, 0x00, 0x00, 0x02}};
  rte_ether_addr_copy(&src_addr, &eth_hdr->src_addr);
  rte_ether_addr_copy(&dst_addr, &eth_hdr->dst_addr);

  // ip header
  ipv4_hdr->src_addr = htonl(0x01010101);
  ipv4_hdr->dst_addr = htonl(0x02020202);
  ipv4_hdr->total_length =
      rte_cpu_to_be_16(pkt_len - sizeof(struct rte_ether_hdr));
  ipv4_hdr->version_ihl = 0x45;
  ipv4_hdr->type_of_service = 0;
  ipv4_hdr->packet_id = 0;
  ipv4_hdr->fragment_offset = 0;
  ipv4_hdr->time_to_live = 64;
  ipv4_hdr->next_proto_id = IPPROTO_UDP;

  uint16_t ipv4_checksum = rte_ipv4_cksum(ipv4_hdr);
  ipv4_hdr->hdr_checksum = ipv4_checksum;

  // udp header
  udp_hdr->src_port = htons(8000);
  udp_hdr->dst_port = htons(8000);
  udp_hdr->dgram_len = rte_cpu_to_be_16(pkt_len - sizeof(struct rte_ether_hdr) -
                                        sizeof(struct rte_ipv4_hdr));
  udp_hdr->dgram_cksum = 0;
}

void app_init() {
  fprintf(stdout, "App init\n");

  int ret;

  fprintf(stdout, "Creating mbuf pool...\n");
  pktmbuf_pool =
      rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

  if (NULL == pktmbuf_pool) {
    rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
  }
  fprintf(stdout, "Mbuf pool created\n");

  n_lcores = rte_lcore_count();
  fprintf(stdout, "Number of lcores: %d\n", n_lcores);

  // TODO: currently only use 1 port
  uint32_t tx_queues_per_port = n_lcores;

  // Initialize port
  uint32_t port_id = 0;

  uint32_t vid = 0;
  uint32_t tx_queue_id = 0;
  for (int i = 0; i < RTE_MAX_LCORE; i++) {
    if (rte_lcore_is_enabled(i)) {
      lcore_conf[i].vid = vid++;
      lcore_conf[i].port = port_id;
      lcore_conf[i].tx_queue_id = tx_queue_id++;
      lcore_conf[i].tx_mbufs.len = 0;
    }
  }

  if (!rte_eth_dev_is_valid_port(port_id)) {
    fprintf(stderr, "Invalid port %d\n", port_id);
    rte_exit(EXIT_FAILURE, "Invalid port %d\n", port_id);
  }

  struct rte_eth_conf port_conf;
  struct rte_eth_dev_info dev_info;
  struct rte_eth_txconf txconf;
  uint16_t nb_rxd = RX_RING_SIZE;
  uint16_t nb_txd = TX_RING_SIZE;

  memset(&port_conf, 0, sizeof(struct rte_eth_dev_info));

  ret = rte_eth_dev_info_get(port_id, &dev_info);
  if (ret != 0) {
    fprintf(stderr, "Error during getting device (port %u) info: %s\n", port_id,
            strerror(-ret));
    rte_exit(EXIT_FAILURE, "Cannot get device (port %u) info: %s\n", port_id,
             strerror(-ret));
  }

  if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
    port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
  }

  ret = rte_eth_dev_configure(port_id, RX_QUEUES_PER_PORT, tx_queues_per_port,
                              &port_conf);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n", ret,
             port_id);
  }

  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id, &nb_rxd, &nb_txd);
  if (ret != 0) {
    rte_exit(EXIT_FAILURE,
             "Cannot adjust number of descriptors: err=%d, port=%u\n", ret,
             port_id);
  }

  for (int i = 0; i < RX_QUEUES_PER_PORT; i++) {
    ret = rte_eth_rx_queue_setup(
        port_id, i, nb_rxd, rte_eth_dev_socket_id(port_id), NULL, pktmbuf_pool);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE, "Cannot setup RX queue %d\n", i);
    }
  }

  txconf = dev_info.default_txconf;
  txconf.offloads = port_conf.txmode.offloads;
  for (int i = 0; i < tx_queues_per_port; i++) {
    ret = rte_eth_tx_queue_setup(port_id, i, nb_txd,
                                 rte_eth_dev_socket_id(port_id), &txconf);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE, "Cannot setup TX queue %d\n", i);
    }
  }

  ret = rte_eth_dev_start(port_id);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Cannot start port %d\n", port_id);
  }

  struct rte_ether_addr eth_addr;
  ret = rte_eth_macaddr_get(port_id, &eth_addr);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Cannot get MAC address: err=%d, port=%u\n", ret,
             port_id);
  }

  fprintf(stdout,
          "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
          " %02" PRIx8 " %02" PRIx8 "\n",
          port_id, RTE_ETHER_ADDR_BYTES(&eth_addr));

  ret = rte_eth_promiscuous_enable(port_id);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Cannot enable promiscuous mode: err=%d, port=%u\n",
             ret, port_id);
  }
  init_header_template();
}

void send_pcakets(uint32_t lcore_id) {
  struct lcore_configuration *conf = &lcore_conf[lcore_id];

  uint32_t len = conf->tx_mbufs.len;
  struct rte_mbuf **m_table = (struct rte_mbuf **)conf->tx_mbufs.m_table;

  int ret;

  ret = rte_eth_tx_burst(conf->port, conf->tx_queue_id, m_table, len);
  // TODO: here should use bits
  tput_stat[conf->vid].tx_bits += ret * 8 * (sizeof(header_template) + sizeof(struct MessageHeader));
  if (unlikely(ret < len)) {
    tput_stat[conf->vid].dropped_pkts += len - ret;
    do {
      rte_pktmbuf_free(m_table[ret]);
    } while (++ret < len);
  }
  conf->tx_mbufs.len = 0;
}

void print_per_core_throughput(uint32_t seconds) {
  fprintf(stdout, "%6" PRIu32 " seconds\n", seconds);

  uint64_t total_tx_bits = 0;
  uint64_t total_dropped_pkts = 0;

  for (int i = 0; i < n_lcores; i++) {
    struct throughput_statistics *stat = &tput_stat[i];
    uint64_t tx_bits = stat->tx_bits - stat->last_tx_bits;
    uint64_t dropped_pkts = stat->dropped_pkts - stat->last_dropped_pkts;
    fprintf(stdout,
            "\tcore %" PRIu32 "\ttx_bits: %" PRIu64 "\tdropped_pkts: %" PRIu64
            "\n",
            i, tx_bits, dropped_pkts);

    stat->last_tx_bits = stat->tx_bits;
    stat->last_dropped_pkts = stat->dropped_pkts;

    total_tx_bits += tx_bits;
    total_dropped_pkts += dropped_pkts;
  }

  fprintf(stdout, "\ttotal\ttx_bits: %" PRIu64 "\tdropped_pkts: %" PRIu64 "\n",
          total_tx_bits, total_dropped_pkts);

  fflush(stdout);
}

void enqueue_packet(uint32_t lcore_id, struct rte_mbuf *pkt) {
  struct lcore_configuration *conf = &lcore_conf[lcore_id];
  conf->tx_mbufs.m_table[conf->tx_mbufs.len++] = pkt;

  if (unlikely(conf->tx_mbufs.len == MAX_BURST_SIZE)) {
    send_pcakets(lcore_id);
  }
}

void generate_packet(struct rte_mbuf *mbuf) {
  assert(mbuf != NULL);

  struct rte_ether_hdr *eth_hdr =
      rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
  struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
  struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);

  rte_memcpy(eth_hdr, header_template, sizeof(header_template));

  struct MessageHeader *msg_hdr = (struct MessageHeader *)(udp_hdr + 1);
  // ip_hdr->src_addr = htonl(0x01010101);
  // ip_hdr->dst_addr = htonl(0x01010101);

  udp_hdr->src_port = htons(1234);
  udp_hdr->dst_port = htons(5678);

  msg_hdr->seq_num = 1;
  msg_hdr->rank = 2;

  mbuf->data_len = sizeof(header_template) + sizeof(struct MessageHeader);
  mbuf->pkt_len = mbuf->data_len;
  mbuf->next = NULL;
  mbuf->nb_segs = 1;
  mbuf->ol_flags = 0;

}

void lcore_main() {
  uint32_t lcore_id = rte_lcore_id();
  fprintf(stdout, "lcore %u started\n", lcore_id);

  uint64_t now_tsc = rte_rdtsc();
  uint64_t tsc_hz = rte_get_tsc_hz();
  uint64_t next_update_tsc = now_tsc + tsc_hz;
  uint32_t seconds_cnt = 0;

  uint64_t tsc_ms = tsc_hz / MS_PER_S;
  uint64_t next_ms_tsc = now_tsc + tsc_ms;

  uint64_t drain_tsc = (tsc_hz + US_PER_S - 1) / US_PER_S * 10;
  uint64_t next_drain_tsc = now_tsc + drain_tsc;

  uint64_t finish_tsc = now_tsc + time_to_run * tsc_hz;

  uint64_t pkts_send_ms_cnt = 0;

  struct rte_mbuf *mbuf;

  while (1) {
    now_tsc = rte_rdtsc();

    // finished
    if (unlikely(now_tsc > finish_tsc)) {
      fprintf(stdout, "Finished\n");
      break;
    }

    if (unlikely(now_tsc > next_update_tsc)) {
      if (lcore_id == rte_get_main_lcore()) {
        print_per_core_throughput(seconds_cnt);
      }
      next_update_tsc += tsc_hz;
      seconds_cnt++;
    }

    if (unlikely(now_tsc > next_ms_tsc)) {
      pkts_send_ms_cnt = 0;
      next_ms_tsc += tsc_ms;
    }

    if (unlikely(now_tsc > next_drain_tsc)) {
      send_pcakets(lcore_id);
      next_drain_tsc += drain_tsc;
    }

    if ((now_tsc <= finish_tsc) &&
        (pkts_send_ms_cnt < pkts_send_limit_per_ms)) {
      pkts_send_ms_cnt++;
      mbuf = rte_pktmbuf_alloc(pktmbuf_pool);
      generate_packet(mbuf);
      enqueue_packet(lcore_id, mbuf);
    }
  }
}

int app_parse_args(int argc, char **argv) {
  int opt;
  long long num;

  while (-1 != (opt = getopt(argc, argv, "s:T:"))) {
    switch (opt) {
      case 's':
        num = atoi(optarg);
        pkts_send_limit_per_ms = num;
        break;
      case 'T':
        num = atoi(optarg);
        time_to_run = num;
        break;
      default:
        fprintf(stderr, "unknown option %c\n", opt);
        return -1;
    }
  }

  return 0;
}

int main(int argc, char **argv) {
  int ret;

  ret = rte_eal_init(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "EAL init failed\n");
  }
  argc -= ret;
  argv += ret;

  ret = app_parse_args(argc, argv);
  if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Invalid arguments\n");
  }

  app_init();

  rte_eal_cleanup();

  return 0;
}