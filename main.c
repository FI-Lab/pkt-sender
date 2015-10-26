#include <stdio.h>
#include <netinet/in.h>
#include <stddef.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_memory.h>
#include <rte_mempool.h>
#include <rte_eal.h>
#include <rte_debug.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_ethdev.h>

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>

#include "pm.h"
#include "tload.h"

/* global data */
struct 
{
    uint32_t total_trace;
}global_data;

/* generate mbuf */
#define NB_MAX_PM 1000001
struct packet_model pms[NB_MAX_PM];

static struct rte_mbuf* generate_mbuf(struct packet_model pm, struct rte_mempool *mp)
{
    struct rte_mbuf *m;

    m = rte_pktmbuf_alloc(mp);
    if(m == NULL)
    {
        rte_exit(-1, "mempool is empty!\n");
    }
    char *data;
    data = rte_pktmbuf_append(m, sizeof(struct ether_hdr));
    rte_memcpy(data, &(pm.eth), sizeof(struct ether_hdr));
    data = rte_pktmbuf_append(m, sizeof(struct ipv4_hdr));
    rte_memcpy(data, &(pm.ip), sizeof(struct ipv4_hdr));
    if(pm.is_udp)
    {
        data = rte_pktmbuf_append(m, sizeof(struct udp_hdr));
        rte_memcpy(data, &(pm.l4.udp.hdr), sizeof(struct udp_hdr));
        data = rte_pktmbuf_append(m, 18);
        rte_memcpy(data, pm.l4.udp.payload, 18);
    }
    else
    {
        data = rte_pktmbuf_append(m, sizeof(struct tcp_hdr));
        rte_memcpy(data, &(pm.l4.tcp.hdr), sizeof(struct tcp_hdr));
        data = rte_pktmbuf_append(m, 6);
        rte_memcpy(data, pm.l4.tcp.payload, 6);
    }
    return m;
}

/* params decode */
struct
{
    char trace_file[256];
}params;

static void usage()
{
    printf("Usage: pkt-sender <EAL options> -- -t <trace_file>\n");
    rte_exit(-1, "invalid arguments!\n");
}

static void parse_params(int argc, char **argv)
{
    char opt;
    int accept = 0;
    while((opt = getopt(argc, argv, "t:")) != -1)
    {
        switch(opt)
        {
            case 't': rte_memcpy(params.trace_file, optarg, strlen(optarg)+1); accept = 1; break;
            default: usage();
        }
    }
    if(!accept)
    {
        usage();
    }
}
/**************************************************************/

/* port init */

const int nb_rxd = 128;
const int nb_txd = 512;

#define NB_MBUF 8191
#define MBUF_CACHE_SIZE 128
#define NB_BURST 32

#define NB_TXQ 3
#define NB_RXQ 3

struct rte_eth_conf port_conf = 
{
    .rxmode = 
    {
        .max_rx_pkt_len = ETHER_MAX_LEN,
    },
};

/* don't care about NUMA */
static int all_port_setup(struct rte_mempool *mp)
{
    int ret;
    int nb_ports;
    int port_id;
    int queue_id;
    nb_ports = rte_eth_dev_count();

    for(port_id = 0; port_id < nb_ports; port_id++)
    {
        ret = rte_eth_dev_configure(port_id, NB_RXQ, NB_TXQ, &port_conf);
        if(ret < 0)
        {
            rte_exit(-1, "port %d configure failure!\n", port_id);
        }
        for(queue_id = 0; queue_id < NB_TXQ; queue_id++)
        {
            ret = rte_eth_tx_queue_setup(port_id, queue_id, nb_txd, rte_eth_dev_socket_id(port_id), NULL);
            if(ret != 0)
            {
                rte_exit(-1, "port %d tx queue setup failure!\n", port_id);
            }
        }
        for(queue_id = 0; queue_id < NB_RXQ; queue_id++)
        {
            ret = rte_eth_rx_queue_setup(port_id, queue_id, nb_rxd, rte_eth_dev_socket_id(port_id), NULL, mp);
            if(ret != 0)
            {
                rte_exit(-1, "port %d rx queue setup failure!\n", port_id);
            }
        }
        ret = rte_eth_dev_start(port_id);
        if(ret < 0)
        {
            rte_exit(-1, "port %d start failure!\n", port_id);
        }
        rte_eth_promiscuous_enable(port_id);
    }
    return nb_ports;
}


/* lcore main */

struct
{
    struct
    {
        uint64_t tx_total_pkts;
        uint64_t tx_last_total_pkts;
    }txq_stats[NB_TXQ];

    struct
    {
        uint64_t rx_total_pkts;
        uint64_t rx_last_total_pkts;
    }rxq_stats[NB_RXQ];
}port_stats[RTE_MAX_ETHPORTS];

struct lcore_args
{
    struct rte_mempool *mp;
    uint32_t port_id;
    uint32_t queue_id;
};

static int sender_lcore_main(__attribute__((unused)) void *args)
{
    struct rte_mempool *mp;
    struct rte_mbuf *m_table[NB_BURST];
    struct rte_mbuf *rx_tlb[NB_BURST];
    struct lcore_args *largs;
    uint32_t port_id;
    uint32_t queue_id;

    largs = (struct lcore_args*)args;
    
    mp = largs->mp;
    port_id = largs->port_id;
    queue_id = largs->queue_id;
    
    int i, count, ret;
    printf("send packet from port %u - queue %u!\n", port_id, queue_id);

    port_stats[port_id].txq_stats[queue_id].tx_total_pkts = 0;
    port_stats[port_id].txq_stats[queue_id].tx_last_total_pkts = 0;

    port_stats[port_id].rxq_stats[queue_id].rx_total_pkts = 0;
    port_stats[port_id].rxq_stats[queue_id].rx_last_total_pkts = 0;


    for(i = 0, count = 0;;)
    {
        ret = rte_eth_rx_burst(port_id, queue_id, rx_tlb, NB_BURST);
        port_stats[port_id].rxq_stats[queue_id].rx_total_pkts += ret;

        while(ret > 0)
        {
            rte_pktmbuf_free(rx_tlb[--ret]);
        }

        m_table[count++] = generate_mbuf(pms[i++], mp);
        if(i == global_data.total_trace)
        {
            i = 0;
        }
        if((count % NB_BURST) == 0)
        {
            ret = rte_eth_tx_burst(port_id, queue_id, m_table, NB_BURST);
            port_stats[port_id].txq_stats[queue_id].tx_total_pkts += ret;
            while(ret < NB_BURST)
            {
                rte_pktmbuf_free(m_table[ret++]);
            }
            count = 0;
        }
    }
}

static void print_stats(int nb_ports)
{
    int i, j;
    uint64_t tx_total;
    uint64_t tx_last_total;
    uint64_t rx_total;
    uint64_t rx_last_total;
    uint64_t tx_pps;
    uint64_t tx_mbps;
    uint64_t rx_pps;
    uint64_t rx_mbps;
    for(;;)
    {
        sleep(5);
        i = system("clear");
        for(i = 0; i < nb_ports; i++)
        {
            tx_total = tx_last_total = 0;
            rx_total = rx_last_total = 0;
            for(j = 0; j < NB_TXQ; j++)
            {
                tx_total += port_stats[i].txq_stats[j].tx_total_pkts;
                tx_last_total += port_stats[i].txq_stats[j].tx_last_total_pkts;
                port_stats[i].txq_stats[j].tx_last_total_pkts = port_stats[i].txq_stats[j].tx_total_pkts;
            }
            for(j = 0; j < NB_RXQ; j++)
            {
                rx_total += port_stats[i].rxq_stats[j].rx_total_pkts;
                rx_last_total += port_stats[i].rxq_stats[j].rx_last_total_pkts;
                port_stats[i].rxq_stats[j].rx_last_total_pkts = port_stats[i].rxq_stats[j].rx_total_pkts;
            }
            tx_pps = (tx_total - tx_last_total) / 5;
            tx_mbps = tx_pps * 84 * 8 / 1000000;
            rx_pps = (rx_total - rx_last_total) / 5;
            rx_mbps = rx_pps * 84 * 8 / 1000000;    
            printf("Port %d Statistics:\n", i);
            printf(">>>>>>>>>>>tx rate: %llupps\n", (unsigned long long)tx_pps);
            printf(">>>>>>>>>>>tx rate: %lluMbps\n", (unsigned long long)tx_mbps);
            printf(">>>>>>>>>>tx total: %llu\n", (unsigned long long)tx_total);
            printf("\n");
            printf(">>>>>>>>>>>rx rate: %llupps\n", (unsigned long long)rx_pps);
            printf(">>>>>>>>>>>rx rate: %lluMbps\n", (unsigned long long)rx_mbps);
            printf(">>>>>>>>>>rx total: %llu\n", (unsigned long long)rx_total);
            printf("============================\n");

        }
    }
}


int main(int argc, char **argv)
{
    int ret;
    ret = rte_eal_init(argc, argv);
    if(ret < 0)
    {
        rte_exit(-1, "rte_eal_init failure!\n");
    }
    
    struct rte_mempool *mbuf_pool;
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NB_MBUF, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if(mbuf_pool == NULL)
    {
        rte_exit(-1, "create pktmbuf pool failure!\n");
    }
    int nb_ports;
    nb_ports = all_port_setup(mbuf_pool);
    if(nb_ports <= 0)
    {
        rte_exit(-1, "not detect any DPDK devices!\n");
    }

    int lcore_nb;
    lcore_nb = rte_lcore_count();

    if(lcore_nb < nb_ports * NB_TXQ + 1)
    {
        rte_exit(-1, "lcore is less than needed! (should be %d)\n", nb_ports + 1);
    }
    if(lcore_nb > nb_ports * NB_TXQ + 1)
    {
        rte_exit(-1, "lcore is too much! (should be %d)\n", nb_ports + 1);
    }

    parse_params(argc - ret, argv + ret);
    ret = load_trace(params.trace_file, pms);   
    global_data.total_trace = ret;
    if(ret <= 0)
    {
        rte_exit(-1, "no invalid trace!\n");
    }

    uint32_t lcore_id;
    uint32_t port_id;
    uint32_t queue_id;
    port_id = queue_id = 0;
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        struct lcore_args largs;
        largs.mp = mbuf_pool;
        largs.queue_id = queue_id % NB_TXQ;
        largs.port_id = queue_id++ / NB_TXQ;
        rte_eal_remote_launch(sender_lcore_main, (void*)&largs, lcore_id);
    }
    print_stats(nb_ports);
    rte_eal_mp_wait_lcore();
    return 0;
}
