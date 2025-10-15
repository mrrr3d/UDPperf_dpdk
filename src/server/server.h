#ifndef SERVER_H
#define SERVER_H

#include <rte_common.h>
#include <rte_ether.h>
#include <rte_ip4.h>
#include <rte_lcore.h>
#include <rte_memory.h>
#include <rte_udp.h>
#include <stdint.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define TX_RING_SIZE 1024
#define RX_RING_SIZE 1024
#define TX_QUEUES_PER_PORT 1
#define MAX_BURST_SIZE 32

struct mbuf_table {
  uint32_t len;
  struct rte_mbuf *m_table[MAX_BURST_SIZE];
};

struct lcore_configuration {
  uint32_t vid;
  uint16_t port;
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

// void init_header_template(void);
// void app_init(void);
// void send_pcakets(uint32_t lcore_id);
// void print_per_core_throughput(uint32_t seconds);
// void enqueue_packet(uint32_t lcore_id, struct rte_mbuf *pkt);
// void generate_packet(struct rte_mbuf *mbuf);
// int lcore_main(void *arg);
// int app_parse_args(int argc, char **argv);

#endif  // SERVER_H
