// SPDX-License-Identifier: GPL-2.0-only
// Drop exactly the first downstream packet covering one application record.
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/pkt_cls.h>
#include <linux/tcp.h>
#include <linux/udp.h>

#ifndef TARGET_PORT
#define TARGET_PORT 9000
#endif
#ifndef TARGET_RECORD
#define TARGET_RECORD 4096
#endif
#ifndef TARGET_TCP
#define TARGET_TCP 0
#endif

struct drop_state {
    __u32 tcp_base;
    __u64 dropped;
    __u32 packet_sequence;
    __u32 packet_payload_bytes;
    __u64 dropped_at_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct drop_state);
} rudp_drop_state SEC(".maps");

static __always_inline int load_u32(struct __sk_buff *skb, __u32 offset, __u32 *value)
{
    __be32 network;
    if (bpf_skb_load_bytes(skb, offset, &network, sizeof(network)) != 0)
        return -1;
    *value = bpf_ntohl(network);
    return 0;
}

SEC("classifier")
int drop_record_once(struct __sk_buff *skb)
{
    __u32 zero = 0, ip_offset = sizeof(struct ethhdr), transport, sequence, target;
    __u16 protocol, destination;
    __u8 ihl, tcp_length, flags, rudp_type;
    struct drop_state *state = bpf_map_lookup_elem(&rudp_drop_state, &zero);

    if (!state || bpf_skb_load_bytes(skb, 12, &protocol, sizeof(protocol)) != 0 ||
        protocol != bpf_htons(ETH_P_IP) ||
        bpf_skb_load_bytes(skb, ip_offset, &ihl, sizeof(ihl)) != 0)
        return TC_ACT_OK;
    transport = ip_offset + (__u32)(ihl & 0x0f) * 4;
    if (bpf_skb_load_bytes(skb, transport + 2, &destination, sizeof(destination)) != 0 ||
        destination != bpf_htons(TARGET_PORT))
        return TC_ACT_OK;

#if TARGET_TCP
    if (bpf_skb_load_bytes(skb, transport + 13, &flags, sizeof(flags)) != 0 ||
        load_u32(skb, transport + 4, &sequence) != 0)
        return TC_ACT_OK;
    if ((flags & 0x02) != 0) {
        state->tcp_base = sequence + 1;
        return TC_ACT_OK;
    }
    if (bpf_skb_load_bytes(skb, transport + 12, &tcp_length, sizeof(tcp_length)) != 0)
        return TC_ACT_OK;
    tcp_length = (__u8)((tcp_length >> 4) * 4);
    if ((__u32)skb->len <= transport + tcp_length || state->tcp_base == 0)
        return TC_ACT_OK;
    target = state->tcp_base + 28 + TARGET_RECORD * 1024;
    if (sequence > target || sequence + ((__u32)skb->len - transport - tcp_length) <= target)
        return TC_ACT_OK;
#else
    if (bpf_skb_load_bytes(skb, transport + sizeof(struct udphdr) + 1, &rudp_type,
                           sizeof(rudp_type)) != 0 ||
        rudp_type != 5 || load_u32(skb, transport + sizeof(struct udphdr) + 20, &sequence) != 0 ||
        sequence != TARGET_RECORD)
        return TC_ACT_OK;
    tcp_length = sizeof(struct udphdr);
#endif
    if (__sync_val_compare_and_swap(&state->dropped, 0, 1) != 0)
        return TC_ACT_OK;
    state->packet_sequence = sequence;
    state->packet_payload_bytes = (__u32)skb->len - transport - tcp_length;
    state->dropped_at_ns = bpf_ktime_get_ns();
    return TC_ACT_SHOT;
}

char LICENSE[] SEC("license") = "GPL";
