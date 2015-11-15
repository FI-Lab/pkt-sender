#ifndef PM_H
#define PM_H

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>

struct packet_model
{
    struct
    {
        struct ether_hdr eth;
        struct ipv4_hdr ip;
        struct tcp_hdr tcp;
    }__attribute__((__packed__)) tcp;
    struct
    {
        struct ether_hdr eth;
        struct ipv4_hdr ip;
        struct udp_hdr udp;
    }__attribute__((__packed__)) udp;
    int is_udp;
};

#endif
