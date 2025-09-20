#include <rte_build_config.h>
#include <rte_common.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define TX_RING_SIZE 1024
#define RX_RING_SIZE 1024
#define RX_QUEUES_PER_PORT 2

uint64_t pkts_send_limit_per_ms = 800;
uint64_t time_to_run = 10;

struct rte_mempool *pktmbuf_pool;

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

  uint32_t n_lcores = rte_lcore_count();
  fprintf(stdout, "Number of lcores: %d\n", n_lcores);

  // TODO: currently only use 1 port
  uint32_t tx_queues_per_port = n_lcores;

  // Initialize port
  uint32_t port_id = 0;

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