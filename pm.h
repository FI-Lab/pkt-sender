#ifndef PM_H
#define PM_H

#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>

struct packet_model
{
    struct ether_hdr eth;
    struct ipv4_hdr ip;
    union
    {
        struct
        {
            struct udp_hdr hdr;
            uint8_t payload[18];
        }udp;
        struct
        {
            struct tcp_hdr hdr;
            uint8_t payload[6];
        }tcp;
    }l4;
    int is_udp;
};

#endif
