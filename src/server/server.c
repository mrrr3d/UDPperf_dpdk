#include "server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <generic/rte_cycles.h>
#include <rte_branch_prediction.h>
#include <rte_build_config.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct lcore_configuration lcore_conf[RTE_MAX_LCORE];
struct throughput_statistics tput_stat[RTE_MAX_LCORE];

uint8_t enabled_ports[RTE_MAX_ETHPORTS];
uint32_t n_enabled_ports;

uint8_t header_template[sizeof(struct rte_ether_hdr) +
                        sizeof(struct rte_ipv4_hdr) +
                        sizeof(struct rte_udp_hdr)];

uint32_t n_lcores;
struct rte_mempool *pktmbuf_pool;

// args
uint64_t time_to_run = 20;
uint16_t num_ports = 1;

void init_header_template(void) {
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
void app_init(void) {
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

  uint16_t n_avail_ports = rte_eth_dev_count_avail();
  fprintf(stdout, "Number of ports available: %d\n", n_avail_ports);
  if (n_avail_ports < num_ports) {
    fprintf(stderr, "Available ports: %d, required ports: %d\n", n_avail_ports,
            num_ports);
    rte_exit(EXIT_FAILURE, "not enough available ports\n");
  }

  if ((n_lcores - 1) % num_ports) {
    rte_exit(EXIT_FAILURE,
             "(n_lcores - 1) should be a multiple of number of ports, n_lcores"
             "=%u, num_ports=%u\n",
             n_lcores, num_ports);
  }

  memset(enabled_ports, 0, sizeof(enabled_ports));
  n_enabled_ports = 0;
  for (uint16_t i = 0; i < RTE_MAX_ETHPORTS; i++) {
    if (rte_eth_dev_is_valid_port(i)) {
      enabled_ports[i] = 1;
      n_enabled_ports++;
    }
    if (n_enabled_ports >= num_ports) {
      break;
    }
  }

  uint32_t rx_queues_per_port = (n_lcores - 1) / num_ports;

  uint32_t vid = 0;
  uint32_t rx_queue_id = 0;
  uint16_t porti = 0;
  uint32_t main_lcore_id = rte_get_main_lcore();

  while (porti < RTE_MAX_ETHPORTS && enabled_ports[porti] == 0) porti++;
  for (uint32_t i = 0; i < RTE_MAX_LCORE; i++) {
    if (rte_lcore_is_enabled(i)) {
      lcore_conf[i].vid = vid++;
      lcore_conf[i].tx_mbufs.len = 0;

      // skip the main lcore
      if (i != main_lcore_id) {
        lcore_conf[i].rx_queue_id = rx_queue_id++;
        lcore_conf[i].port = porti;

        if (rx_queue_id >= rx_queues_per_port) {
          rx_queue_id = 0;
          porti++;
          while (porti < RTE_MAX_ETHPORTS && enabled_ports[porti] == 0) porti++;
        }
      }
    }
  }

  // Initialize ports
  for (porti = 0; porti < RTE_MAX_ETHPORTS; porti++) {
    if (enabled_ports[porti] == 0) {
      continue;
    }

    if (!rte_eth_dev_is_valid_port(porti)) {
      fprintf(stderr, "Invalid port %d\n", porti);
      rte_exit(EXIT_FAILURE, "Invalid port %d\n", porti);
    }

    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf txconf;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;

    memset(&port_conf, 0, sizeof(struct rte_eth_dev_info));

    ret = rte_eth_dev_info_get(porti, &dev_info);
    if (ret != 0) {
      fprintf(stderr, "Error during getting device (port %u) info: %s\n", porti,
              strerror(-ret));
      rte_exit(EXIT_FAILURE, "Cannot get device (port %u) info: %s\n", porti,
               strerror(-ret));
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
      port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
    }

    ret = rte_eth_dev_configure(porti, rx_queues_per_port, TX_QUEUES_PER_PORT,
                                &port_conf);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE, "Cannot configure device: err=%s, port=%u\n",
               rte_strerror(ret), porti);
    }

    ret = rte_eth_dev_adjust_nb_rx_tx_desc(porti, &nb_rxd, &nb_txd);
    if (ret != 0) {
      rte_exit(EXIT_FAILURE,
               "Cannot adjust number of descriptors: err=%d, port=%u\n", ret,
               porti);
    }

    for (uint32_t i = 0; i < rx_queues_per_port; i++) {
      ret = rte_eth_rx_queue_setup(
          porti, i, nb_rxd, rte_eth_dev_socket_id(porti), NULL, pktmbuf_pool);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup RX queue %d\n", i);
      }
    }

    txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    for (uint32_t i = 0; i < TX_QUEUES_PER_PORT; i++) {
      ret = rte_eth_tx_queue_setup(porti, i, nb_txd,
                                   rte_eth_dev_socket_id(porti), &txconf);
      if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup TX queue %d\n", i);
      }
    }

    ret = rte_eth_dev_start(porti);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE, "Cannot start port %d\n", porti);
    }

    struct rte_ether_addr eth_addr;
    ret = rte_eth_macaddr_get(porti, &eth_addr);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE, "Cannot get MAC address: err=%d, port=%u\n", ret,
               porti);
    }

    fprintf(stdout,
            "Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " %02" PRIx8
            " %02" PRIx8 " %02" PRIx8 "\n",
            porti, RTE_ETHER_ADDR_BYTES(&eth_addr));

    ret = rte_eth_promiscuous_enable(porti);
    if (ret < 0) {
      rte_exit(EXIT_FAILURE,
               "Cannot enable promiscuous mode: err=%d, port=%u\n", ret, porti);
    }
  }
  init_header_template();
}

int app_parse_args(int argc, char **argv) {
  int opt;
  long long num;

  while (-1 != (opt = getopt(argc, argv, "T:P:"))) {
    switch (opt) {
      case 'T':
        num = atoi(optarg);
        time_to_run = num;
        break;
      case 'P':
        num = atoi(optarg);
        num_ports = num;
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

  // rte_eal_mp_remote_launch(lcore_loop, NULL, CALL_MAIN);

  rte_eal_cleanup();

  return 0;
}
