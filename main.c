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
#include <rte_timer.h>
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


const int nb_rxd = 128;
const int nb_txd = 512;

#define NB_MBUF 65535
#define MBUF_CACHE_SIZE 128
#define NB_BURST 32

#define NB_TXQ 3
#define NB_RXQ 1

#define NB_MAX_PM 1000001

#define TX_10Gbps 10000
#define TX_RATE_DFT TX_10Gbps

#define PRINT_GAP 2

/* global data */
struct 
{
    uint32_t total_trace;
}global_data;

/* generate mbuf */
struct packet_model pms[NB_MAX_PM];

/* pkt length*/
extern uint32_t pkt_length;

static inline struct rte_mbuf* generate_mbuf(struct packet_model pm, struct rte_mempool *mp)
{
    struct rte_mbuf *m;

    m = rte_pktmbuf_alloc(mp);
    if(m == NULL)
    {
        rte_exit(-1, "mempool is empty!\n");
    }
    char *data;
   if(pm.is_udp)
    {
        //printf("%d \n", ntohs(pm.udp.ip.total_length));
        data = rte_pktmbuf_append(m, sizeof(pm.udp));
        rte_memcpy(data, &(pm.udp), sizeof(pm.udp));
        data = rte_pktmbuf_append(m, pkt_length - 46);
    }
    else
    {
        //printf("%d \n", ntohs(pm.tcp.ip.total_length));
        data = rte_pktmbuf_append(m, sizeof(pm.tcp));
        rte_memcpy(data, &(pm.tcp), sizeof(pm.tcp));
        data = rte_pktmbuf_append(m, pkt_length - 58);
    }
    return m;
}

/* params decode */
struct
{
    char trace_file[256];
    uint64_t Mbps;
}params = 
{
    .Mbps = TX_RATE_DFT,
};

static void usage()
{
    printf("Usage: pkt-sender <EAL options> -- -t <trace_file> -s <Mbps> -L <pkt_length>\n");
    rte_exit(-1, "invalid arguments!\n");
}

static void parse_params(int argc, char **argv)
{
    char opt;
    int accept = 0;
    while((opt = getopt(argc, argv, "t:s:L:")) != -1)
    {
        switch(opt)
        {
            case 't': rte_memcpy(params.trace_file, optarg, strlen(optarg)+1); accept = 1; break;
            case 's': params.Mbps = atoi(optarg); break;
            case 'L': 
                      pkt_length = atoi(optarg);
                      if(pkt_length < 64)
                      {
                          rte_exit(EINVAL, "pkt_length should be no less than 64!\n");
                      }
                      break;
            default: usage();
        }
    }
    if(!accept)
    {
        usage();
    }

    if(params.Mbps == 0) {
        params.Mbps = 1;
    }

}
/**************************************************************/

/* port init */
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

    uint64_t tx_total;
    uint64_t tx_pps;
    uint64_t tx_mbps;
    uint64_t rx_total;
    uint64_t rx_pps;
    uint64_t rx_mbps;
}port_stats[RTE_MAX_ETHPORTS];

struct lcore_args
{
    uint32_t port_id;
    struct
    {
        struct rte_mbuf *m_table[NB_BURST] __rte_cache_aligned;
        struct rte_mempool *mp;
        uint32_t queue_id;
        struct rte_timer tim;
    }tx;
    struct
    {
        uint8_t is_rx_lcore;
    }rx;
    uint64_t speed;
    uint32_t trace_idx;
};

struct lcore_args lc_args[RTE_MAX_LCORE];

static void send_pkt_rate(__rte_unused struct rte_timer *timer, void *arg) 
{
    struct lcore_args *largs = (struct lcore_args*)arg;
    struct rte_mempool *mp;
    uint32_t port_id;
    uint32_t queue_id;
    uint32_t count = 0;
    int ret;

    mp = largs->tx.mp;
    port_id = largs->port_id;
    queue_id = largs->tx.queue_id;

    for(;count < NB_BURST;)
    {
        if(largs->trace_idx == global_data.total_trace) 
        {
            largs->trace_idx = 0;
        }
        largs->tx.m_table[count++] = generate_mbuf(pms[largs->trace_idx++], mp);
    }

    ret = rte_eth_tx_burst(port_id, queue_id, largs->tx.m_table, NB_BURST);
    port_stats[port_id].txq_stats[queue_id].tx_total_pkts += ret;
    while(ret < NB_BURST)
    {
        rte_pktmbuf_free(largs->tx.m_table[ret++]);
    }
}

static uint64_t calc_period(uint64_t speed)
{
    return (uint64_t) (((NB_BURST * (pkt_length + 20) * 8 * rte_get_tsc_hz()) / (double) speed) );
}

static int sender_lcore_main(void *args)
{
    struct rte_mbuf *rx_table[NB_BURST];
    struct lcore_args *largs;
    uint8_t is_rx;
    int ret;

    largs = (struct lcore_args*)args;
    
    is_rx = largs->rx.is_rx_lcore;
    
    int j;
    printf("send packet from port %u - queue %u!\n", largs->port_id, largs->tx.queue_id);

    port_stats[largs->port_id].txq_stats[largs->tx.queue_id].tx_total_pkts = 0;
    port_stats[largs->port_id].txq_stats[largs->tx.queue_id].tx_last_total_pkts = 0;

    rte_timer_init(&largs->tx.tim);
    
    uint64_t period = calc_period(largs->speed);
    printf("period %lu\n", period);
    rte_timer_reset(&largs->tx.tim, period, PERIODICAL, rte_lcore_id(), send_pkt_rate, largs);

    if(is_rx)
    {
        for(j = 0; j < NB_RXQ; j++)
        {
            port_stats[largs->port_id].rxq_stats[j].rx_total_pkts = 0;
            port_stats[largs->port_id].rxq_stats[j].rx_last_total_pkts = 0;
        }
    }

    for(;;) {
        if(is_rx) {
            for(j = 0; j < NB_RXQ; j++) {
                ret = rte_eth_rx_burst(largs->port_id, j, rx_table, NB_BURST);
                port_stats[largs->port_id].rxq_stats[j].rx_total_pkts += ret;
                while(ret > 0) {
                    rte_pktmbuf_free(rx_table[--ret]);
                }
            }
        }
        rte_timer_manage();
    }
}

static void print_stats(int nb_ports)
{
    int i, j;
    uint64_t tx_total;
    uint64_t tx_last_total;
    uint64_t rx_total;
    uint64_t rx_last_total;
    uint64_t last_cyc, cur_cyc;
    double time_diff;
    last_cyc = rte_get_tsc_cycles();
    for(;;)
    {
        sleep(PRINT_GAP);
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
            cur_cyc = rte_get_tsc_cycles();
            time_diff = (cur_cyc - last_cyc) / (double)rte_get_tsc_hz();
            port_stats[i].tx_total = tx_total;
            port_stats[i].tx_pps = (uint64_t)((tx_total - tx_last_total) / time_diff);
            port_stats[i].tx_mbps = port_stats[i].tx_pps * (pkt_length + 20) * 8 / (1000000);
            port_stats[i].rx_total = rx_total;
            port_stats[i].rx_pps = (uint64_t)((rx_total - rx_last_total) / time_diff);
            port_stats[i].rx_mbps = port_stats[i].rx_pps * (pkt_length + 20) * 8 / (1000000);
            
        }
        last_cyc = rte_get_tsc_cycles();
        for(i = 0; i < nb_ports; i++)
        {
            printf("Port %d Statistics:\n", i);
            printf(">>>>>>>>>>>tx rate: %llupps\n", (unsigned long long)port_stats[i].tx_pps);
            printf(">>>>>>>>>>>tx rate: %lluMbps\n", (unsigned long long)port_stats[i].tx_mbps);
            printf(">>>>>>>>>>tx total: %llu\n", (unsigned long long)port_stats[i].tx_total);
            printf("\n");
            printf(">>>>>>>>>>>rx rate: %llupps\n", (unsigned long long)port_stats[i].rx_pps);
            printf(">>>>>>>>>>>rx rate: %lluMbps\n", (unsigned long long)port_stats[i].rx_mbps);
            printf(">>>>>>>>>>rx total: %llu\n", (unsigned long long)port_stats[i].rx_total);
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

    rte_timer_subsystem_init();

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
        lc_args[lcore_id].tx.mp = mbuf_pool;
        lc_args[lcore_id].tx.queue_id = queue_id % NB_TXQ;
        lc_args[lcore_id].rx.is_rx_lcore = lc_args[lcore_id].tx.queue_id == 0? 1: 0; 
        lc_args[lcore_id].port_id = queue_id++ / NB_TXQ;
        lc_args[lcore_id].speed = params.Mbps * 1024 * 1024 / NB_TXQ;
        lc_args[lcore_id].trace_idx = 0;
        rte_eal_remote_launch(sender_lcore_main, (void*)&lc_args[lcore_id], lcore_id);
    }
    print_stats(nb_ports);
    rte_eal_mp_wait_lcore();
    return 0;
}
