#include "tload.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <rte_memory.h>
#include <netinet/in.h>
#include "pm.h"

#if 0
static void debug_pm(struct packet_model pm)
{
    printf("0x0000: %04X %04X %04X %04X %04X %04X %04X %04X\n",
            ((uint16_t)pm.eth.d_addr.addr_bytes[0] << 8) | pm.eth.d_addr.addr_bytes[1],
            ((uint16_t)pm.eth.d_addr.addr_bytes[2] << 8) | pm.eth.d_addr.addr_bytes[3],
            ((uint16_t)pm.eth.d_addr.addr_bytes[4] << 8) | pm.eth.d_addr.addr_bytes[5],
            ((uint16_t)pm.eth.s_addr.addr_bytes[0] << 8) | pm.eth.s_addr.addr_bytes[1],
            ((uint16_t)pm.eth.s_addr.addr_bytes[2] << 8) | pm.eth.s_addr.addr_bytes[3],
            ((uint16_t)pm.eth.s_addr.addr_bytes[4] << 8) | pm.eth.s_addr.addr_bytes[5],
            ((uint16_t)ntohs(pm.eth.ether_type)),
            ((uint16_t)pm.ip.version_ihl << 8) | pm.ip.type_of_service);
    printf("0x0010: %04X %04X %04X %04X %04X %04X %04X %04X\n",
            ntohs(pm.ip.total_length),
            ntohs(pm.ip.packet_id),
            ntohs(pm.ip.fragment_offset),
            ((uint16_t)pm.ip.time_to_live << 8) | pm.ip.next_proto_id,
            ntohs(pm.ip.hdr_checksum),
            (uint16_t)(ntohl(pm.ip.src_addr) >> 16),
            (uint16_t)(ntohl(pm.ip.src_addr) & 0xffff),
            (uint16_t)(ntohl(pm.ip.dst_addr) >> 16));
    printf("0x0020: %04X ", (uint16_t)(ntohl(pm.ip.dst_addr) & 0xffff));
    if(pm.ip.next_proto_id == 6)
    {
        printf("%04X %04X %04X %04X %04X %04X %04X\n",
                ntohs(pm.l4.tcp.hdr.src_port),
                ntohs(pm.l4.tcp.hdr.dst_port),
                ntohl(pm.l4.tcp.hdr.sent_seq) >> 16,
                ntohl(pm.l4.tcp.hdr.sent_seq) & 0xffff,
                ntohl(pm.l4.tcp.hdr.recv_ack) >> 16,
                ntohl(pm.l4.tcp.hdr.recv_ack) & 0xffff,
                ((uint16_t)pm.l4.tcp.hdr.data_off << 8) | pm.l4.tcp.hdr.tcp_flags);
        
        printf("0x0030: %04X %04X %04X\n",
                ntohs(pm.l4.tcp.hdr.rx_win),
                ntohs(pm.l4.tcp.hdr.cksum),
                ntohs(pm.l4.tcp.hdr.tcp_urp));
    }
    else
    {
        printf("%04X %04X %04X %04X\n",
                ntohs(pm.l4.udp.hdr.src_port),
                ntohs(pm.l4.udp.hdr.dst_port),
                ntohs(pm.l4.udp.hdr.dgram_len),
                ntohs(pm.l4.udp.hdr.dgram_cksum));
    }
}
#endif

int load_trace_line(FILE *fp, struct packet_model *pm)
{
    int i;
    char buff[256];
    char *tok[7], *s, *sp;
    if(fgets(buff, 256, fp) == NULL)
    {
        return END_LINE;
    }
    for(i = 0, s = buff; i < NB_FIELD; i++, s = NULL)
    {
        tok[i] = strtok_r(s, " \t\n", &sp);
    }

    uint8_t proto;
    proto = (uint8_t)strtoul(tok[4], NULL, 0);
    if(proto != 6 && proto != 17)
    {
        return INVALID_LINE;
    }
    
    //ether header
    memset(&(pm->eth.d_addr), 0, sizeof(pm->eth.d_addr));
    memset(&(pm->eth.s_addr), 0, sizeof(pm->eth.s_addr));
    pm->eth.d_addr.addr_bytes[5] = (uint8_t)0x02;
    pm->eth.s_addr.addr_bytes[5] = (uint8_t)0x01;
    pm->eth.ether_type = htons((uint16_t)0x0800);

    //ipv4 header
    pm->ip.next_proto_id = proto;
    pm->ip.version_ihl = (uint8_t)0x45;
    pm->ip.type_of_service = (uint8_t)0;
    pm->ip.total_length = htons((uint16_t)46);
    pm->ip.packet_id = 0;
    pm->ip.fragment_offset = 0x0040;//DF
    pm->ip.time_to_live = 0xff;
    pm->ip.hdr_checksum = 0;
    pm->ip.src_addr = htonl(strtoul(tok[0], NULL, 0));
    pm->ip.dst_addr = htonl(strtoul(tok[1], NULL, 0));
    pm->ip.hdr_checksum = rte_ipv4_cksum(&(pm->ip));

    //l4 header
    if(proto == 6)
    {
        pm->l4.tcp.hdr.src_port = htons((uint16_t)strtoul(tok[2], NULL, 0));
        pm->l4.tcp.hdr.dst_port = htons((uint16_t)strtoul(tok[3], NULL, 0));
        pm->l4.tcp.hdr.sent_seq = htonl(1);
        pm->l4.tcp.hdr.recv_ack = htonl(2);
        pm->l4.tcp.hdr.data_off = (uint8_t)(sizeof(struct tcp_hdr)>>2)<<4;
        pm->l4.tcp.hdr.tcp_flags = (uint8_t)0x10;
        pm->l4.tcp.hdr.rx_win = htons(0xffff);
        pm->l4.tcp.hdr.cksum = 0;
        pm->l4.tcp.hdr.tcp_urp = 0;
        pm->l4.tcp.hdr.cksum = rte_ipv4_udptcp_cksum(&(pm->ip), (void*)&(pm->l4.tcp));
        memset(pm->l4.tcp.payload, 0, 6);
        pm->is_udp = 0;
    }
    else
    {
        pm->l4.udp.hdr.src_port = htons((uint16_t)strtoul(tok[2], NULL, 0));
        pm->l4.udp.hdr.dst_port = htons((uint16_t)strtoul(tok[3], NULL, 0));
        pm->l4.udp.hdr.dgram_len = htons(26);
        pm->l4.udp.hdr.dgram_cksum = 0;
        pm->l4.udp.hdr.dgram_cksum = rte_ipv4_udptcp_cksum(&(pm->ip), (void*)&(pm->l4.udp));
        memset(pm->l4.udp.payload, 0, 18);
        pm->is_udp = 1;
    }
    return VALID_LINE;
}

int load_trace(const char *file, struct packet_model pms[])
{
    FILE *fp = fopen(file, "rb");
    int ret = 0;
    int count = 0;
    if(fp == NULL)
    {
        rte_exit(-1, "open trace file failure!\n");
    }
    while((ret = load_trace_line(fp, &pms[count])) != END_LINE)
    {
        if(ret == VALID_LINE)
        {
            count++;
        }
    }
    printf("total trace %d\n", count);
    return count;
}

