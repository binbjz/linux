/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_IP_TUNNELS_H
#define __NET_IP_TUNNELS_H 1

#include <linux/if_tunnel.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <linux/u64_stats_sync.h>
#include <linux/bitops.h>

#include <net/dsfield.h>
#include <net/gro_cells.h>
#include <net/inet_ecn.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#include <net/lwtunnel.h>
#include <net/dst_cache.h>

#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#endif

/* Keep error state on tunnel for 30 sec */
#define IPTUNNEL_ERR_TIMEO	(30*HZ)

/* Used to memset ip_tunnel padding. */
#define IP_TUNNEL_KEY_SIZE	offsetofend(struct ip_tunnel_key, tp_dst)

/* Used to memset ipv4 address padding. */
#define IP_TUNNEL_KEY_IPV4_PAD	offsetofend(struct ip_tunnel_key, u.ipv4.dst)
#define IP_TUNNEL_KEY_IPV4_PAD_LEN				\
	(sizeof_field(struct ip_tunnel_key, u) -		\
	 sizeof_field(struct ip_tunnel_key, u.ipv4))

#define __ipt_flag_op(op, ...)					\
	op(__VA_ARGS__, __IP_TUNNEL_FLAG_NUM)

#define IP_TUNNEL_DECLARE_FLAGS(...)				\
	__ipt_flag_op(DECLARE_BITMAP, __VA_ARGS__)

#define ip_tunnel_flags_zero(...)	__ipt_flag_op(bitmap_zero, __VA_ARGS__)
#define ip_tunnel_flags_copy(...)	__ipt_flag_op(bitmap_copy, __VA_ARGS__)
#define ip_tunnel_flags_and(...)	__ipt_flag_op(bitmap_and, __VA_ARGS__)
#define ip_tunnel_flags_or(...)		__ipt_flag_op(bitmap_or, __VA_ARGS__)

#define ip_tunnel_flags_empty(...)				\
	__ipt_flag_op(bitmap_empty, __VA_ARGS__)
#define ip_tunnel_flags_intersect(...)				\
	__ipt_flag_op(bitmap_intersects, __VA_ARGS__)
#define ip_tunnel_flags_subset(...)				\
	__ipt_flag_op(bitmap_subset, __VA_ARGS__)

struct ip_tunnel_key {
	__be64			tun_id;
	union {
		struct {
			__be32	src;
			__be32	dst;
		} ipv4;
		struct {
			struct in6_addr src;
			struct in6_addr dst;
		} ipv6;
	} u;
	IP_TUNNEL_DECLARE_FLAGS(tun_flags);
	__be32			label;		/* Flow Label for IPv6 */
	u32			nhid;
	u8			tos;		/* TOS for IPv4, TC for IPv6 */
	u8			ttl;		/* TTL for IPv4, HL for IPv6 */
	__be16			tp_src;
	__be16			tp_dst;
	__u8			flow_flags;
};

struct ip_tunnel_encap {
	u16			type;
	u16			flags;
	__be16			sport;
	__be16			dport;
};

/* Flags for ip_tunnel_info mode. */
#define IP_TUNNEL_INFO_TX	0x01	/* represents tx tunnel parameters */
#define IP_TUNNEL_INFO_IPV6	0x02	/* key contains IPv6 addresses */
#define IP_TUNNEL_INFO_BRIDGE	0x04	/* represents a bridged tunnel id */

/* Maximum tunnel options length. */
#define IP_TUNNEL_OPTS_MAX					\
	GENMASK((sizeof_field(struct ip_tunnel_info,		\
			      options_len) * BITS_PER_BYTE) - 1, 0)

#define ip_tunnel_info_opts(info)				\
	_Generic(info,						\
		 const struct ip_tunnel_info * : ((const void *)(info)->options),\
		 struct ip_tunnel_info * : ((void *)(info)->options)\
	)

struct ip_tunnel_info {
	struct ip_tunnel_key	key;
	struct ip_tunnel_encap	encap;
#ifdef CONFIG_DST_CACHE
	struct dst_cache	dst_cache;
#endif
	u8			options_len;
	u8			mode;
	u8			options[] __aligned_largest __counted_by(options_len);
};

/* 6rd prefix/relay information */
#ifdef CONFIG_IPV6_SIT_6RD
struct ip_tunnel_6rd_parm {
	struct in6_addr		prefix;
	__be32			relay_prefix;
	u16			prefixlen;
	u16			relay_prefixlen;
};
#endif

struct ip_tunnel_prl_entry {
	struct ip_tunnel_prl_entry __rcu *next;
	__be32				addr;
	u16				flags;
	struct rcu_head			rcu_head;
};

struct metadata_dst;

/* Kernel-side variant of ip_tunnel_parm */
struct ip_tunnel_parm_kern {
	char			name[IFNAMSIZ];
	IP_TUNNEL_DECLARE_FLAGS(i_flags);
	IP_TUNNEL_DECLARE_FLAGS(o_flags);
	__be32			i_key;
	__be32			o_key;
	int			link;
	struct iphdr		iph;
};

struct ip_tunnel {
	struct ip_tunnel __rcu	*next;
	struct hlist_node hash_node;

	struct net_device	*dev;
	netdevice_tracker	dev_tracker;

	struct net		*net;	/* netns for packet i/o */

	unsigned long	err_time;	/* Time when the last ICMP error
					 * arrived */
	int		err_count;	/* Number of arrived ICMP errors */

	/* These four fields used only by GRE */
	u32		i_seqno;	/* The last seen seqno	*/
	atomic_t	o_seqno;	/* The last output seqno */
	int		tun_hlen;	/* Precalculated header length */

	/* These four fields used only by ERSPAN */
	u32		index;		/* ERSPAN type II index */
	u8		erspan_ver;	/* ERSPAN version */
	u8		dir;		/* ERSPAN direction */
	u16		hwid;		/* ERSPAN hardware ID */

	struct dst_cache dst_cache;

	struct ip_tunnel_parm_kern parms;

	int		mlink;
	int		encap_hlen;	/* Encap header length (FOU,GUE) */
	int		hlen;		/* tun_hlen + encap_hlen */
	struct ip_tunnel_encap encap;

	/* for SIT */
#ifdef CONFIG_IPV6_SIT_6RD
	struct ip_tunnel_6rd_parm ip6rd;
#endif
	struct ip_tunnel_prl_entry __rcu *prl;	/* potential router list */
	unsigned int		prl_count;	/* # of entries in PRL */
	unsigned int		ip_tnl_net_id;
	struct gro_cells	gro_cells;
	__u32			fwmark;
	bool			collect_md;
	bool			ignore_df;
};

struct tnl_ptk_info {
	IP_TUNNEL_DECLARE_FLAGS(flags);
	__be16 proto;
	__be32 key;
	__be32 seq;
	int hdr_len;
};

#define PACKET_RCVD	0
#define PACKET_REJECT	1
#define PACKET_NEXT	2

#define IP_TNL_HASH_BITS   7
#define IP_TNL_HASH_SIZE   (1 << IP_TNL_HASH_BITS)

struct ip_tunnel_net {
	struct net_device *fb_tunnel_dev;
	struct rtnl_link_ops *rtnl_link_ops;
	struct hlist_head tunnels[IP_TNL_HASH_SIZE];
	struct ip_tunnel __rcu *collect_md_tun;
	int type;
};

static inline void ip_tunnel_set_options_present(unsigned long *flags)
{
	IP_TUNNEL_DECLARE_FLAGS(present) = { };

	__set_bit(IP_TUNNEL_GENEVE_OPT_BIT, present);
	__set_bit(IP_TUNNEL_VXLAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_ERSPAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_GTP_OPT_BIT, present);
	__set_bit(IP_TUNNEL_PFCP_OPT_BIT, present);

	ip_tunnel_flags_or(flags, flags, present);
}

static inline void ip_tunnel_clear_options_present(unsigned long *flags)
{
	IP_TUNNEL_DECLARE_FLAGS(present) = { };

	__set_bit(IP_TUNNEL_GENEVE_OPT_BIT, present);
	__set_bit(IP_TUNNEL_VXLAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_ERSPAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_GTP_OPT_BIT, present);
	__set_bit(IP_TUNNEL_PFCP_OPT_BIT, present);

	__ipt_flag_op(bitmap_andnot, flags, flags, present);
}

static inline bool ip_tunnel_is_options_present(const unsigned long *flags)
{
	IP_TUNNEL_DECLARE_FLAGS(present) = { };

	__set_bit(IP_TUNNEL_GENEVE_OPT_BIT, present);
	__set_bit(IP_TUNNEL_VXLAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_ERSPAN_OPT_BIT, present);
	__set_bit(IP_TUNNEL_GTP_OPT_BIT, present);
	__set_bit(IP_TUNNEL_PFCP_OPT_BIT, present);

	return ip_tunnel_flags_intersect(flags, present);
}

static inline bool ip_tunnel_flags_is_be16_compat(const unsigned long *flags)
{
	IP_TUNNEL_DECLARE_FLAGS(supp) = { };

	bitmap_set(supp, 0, BITS_PER_TYPE(__be16));
	__set_bit(IP_TUNNEL_VTI_BIT, supp);

	return ip_tunnel_flags_subset(flags, supp);
}

static inline void ip_tunnel_flags_from_be16(unsigned long *dst, __be16 flags)
{
	ip_tunnel_flags_zero(dst);

	bitmap_write(dst, be16_to_cpu(flags), 0, BITS_PER_TYPE(__be16));
	__assign_bit(IP_TUNNEL_VTI_BIT, dst, flags & VTI_ISVTI);
}

static inline __be16 ip_tunnel_flags_to_be16(const unsigned long *flags)
{
	__be16 ret;

	ret = cpu_to_be16(bitmap_read(flags, 0, BITS_PER_TYPE(__be16)));
	if (test_bit(IP_TUNNEL_VTI_BIT, flags))
		ret |= VTI_ISVTI;

	return ret;
}

static inline void ip_tunnel_key_init(struct ip_tunnel_key *key,
				      __be32 saddr, __be32 daddr,
				      u8 tos, u8 ttl, __be32 label,
				      __be16 tp_src, __be16 tp_dst,
				      __be64 tun_id,
				      const unsigned long *tun_flags)
{
	key->tun_id = tun_id;
	key->u.ipv4.src = saddr;
	key->u.ipv4.dst = daddr;
	memset((unsigned char *)key + IP_TUNNEL_KEY_IPV4_PAD,
	       0, IP_TUNNEL_KEY_IPV4_PAD_LEN);
	key->tos = tos;
	key->ttl = ttl;
	key->label = label;
	ip_tunnel_flags_copy(key->tun_flags, tun_flags);

	/* For the tunnel types on the top of IPsec, the tp_src and tp_dst of
	 * the upper tunnel are used.
	 * E.g: GRE over IPSEC, the tp_src and tp_port are zero.
	 */
	key->tp_src = tp_src;
	key->tp_dst = tp_dst;

	/* Clear struct padding. */
	if (sizeof(*key) != IP_TUNNEL_KEY_SIZE)
		memset((unsigned char *)key + IP_TUNNEL_KEY_SIZE,
		       0, sizeof(*key) - IP_TUNNEL_KEY_SIZE);
}

static inline bool
ip_tunnel_dst_cache_usable(const struct sk_buff *skb,
			   const struct ip_tunnel_info *info)
{
	if (skb->mark)
		return false;

	return !info || !test_bit(IP_TUNNEL_NOCACHE_BIT, info->key.tun_flags);
}

static inline unsigned short ip_tunnel_info_af(const struct ip_tunnel_info
					       *tun_info)
{
	return tun_info->mode & IP_TUNNEL_INFO_IPV6 ? AF_INET6 : AF_INET;
}

static inline __be64 key32_to_tunnel_id(__be32 key)
{
#ifdef __BIG_ENDIAN
	return (__force __be64)key;
#else
	return (__force __be64)((__force u64)key << 32);
#endif
}

/* Returns the least-significant 32 bits of a __be64. */
static inline __be32 tunnel_id_to_key32(__be64 tun_id)
{
#ifdef __BIG_ENDIAN
	return (__force __be32)tun_id;
#else
	return (__force __be32)((__force u64)tun_id >> 32);
#endif
}

#ifdef CONFIG_INET

static inline void ip_tunnel_init_flow(struct flowi4 *fl4,
				       int proto,
				       __be32 daddr, __be32 saddr,
				       __be32 key, __u8 tos,
				       struct net *net, int oif,
				       __u32 mark, __u32 tun_inner_hash,
				       __u8 flow_flags)
{
	memset(fl4, 0, sizeof(*fl4));

	if (oif) {
		fl4->flowi4_l3mdev = l3mdev_master_upper_ifindex_by_index(net, oif);
		/* Legacy VRF/l3mdev use case */
		fl4->flowi4_oif = fl4->flowi4_l3mdev ? 0 : oif;
	}

	fl4->daddr = daddr;
	fl4->saddr = saddr;
	fl4->flowi4_tos = tos;
	fl4->flowi4_proto = proto;
	fl4->fl4_gre_key = key;
	fl4->flowi4_mark = mark;
	fl4->flowi4_multipath_hash = tun_inner_hash;
	fl4->flowi4_flags = flow_flags;
}

int ip_tunnel_init(struct net_device *dev);
void ip_tunnel_uninit(struct net_device *dev);
void  ip_tunnel_dellink(struct net_device *dev, struct list_head *head);
struct net *ip_tunnel_get_link_net(const struct net_device *dev);
int ip_tunnel_get_iflink(const struct net_device *dev);
int ip_tunnel_init_net(struct net *net, unsigned int ip_tnl_net_id,
		       struct rtnl_link_ops *ops, char *devname);
void ip_tunnel_delete_net(struct net *net, unsigned int id,
			  struct rtnl_link_ops *ops,
			  struct list_head *dev_to_kill);

void ip_tunnel_xmit(struct sk_buff *skb, struct net_device *dev,
		    const struct iphdr *tnl_params, const u8 protocol);
void ip_md_tunnel_xmit(struct sk_buff *skb, struct net_device *dev,
		       const u8 proto, int tunnel_hlen);
int ip_tunnel_ctl(struct net_device *dev, struct ip_tunnel_parm_kern *p,
		  int cmd);
bool ip_tunnel_parm_from_user(struct ip_tunnel_parm_kern *kp,
			      const void __user *data);
bool ip_tunnel_parm_to_user(void __user *data, struct ip_tunnel_parm_kern *kp);
int ip_tunnel_siocdevprivate(struct net_device *dev, struct ifreq *ifr,
			     void __user *data, int cmd);
int __ip_tunnel_change_mtu(struct net_device *dev, int new_mtu, bool strict);
int ip_tunnel_change_mtu(struct net_device *dev, int new_mtu);

struct ip_tunnel *ip_tunnel_lookup(struct ip_tunnel_net *itn,
				   int link, const unsigned long *flags,
				   __be32 remote, __be32 local,
				   __be32 key);

void ip_tunnel_md_udp_encap(struct sk_buff *skb, struct ip_tunnel_info *info);
int ip_tunnel_rcv(struct ip_tunnel *tunnel, struct sk_buff *skb,
		  const struct tnl_ptk_info *tpi, struct metadata_dst *tun_dst,
		  bool log_ecn_error);
int ip_tunnel_changelink(struct net_device *dev, struct nlattr *tb[],
			 struct ip_tunnel_parm_kern *p, __u32 fwmark);
int ip_tunnel_newlink(struct net *net, struct net_device *dev,
		      struct nlattr *tb[], struct ip_tunnel_parm_kern *p,
		      __u32 fwmark);
void ip_tunnel_setup(struct net_device *dev, unsigned int net_id);

bool ip_tunnel_netlink_encap_parms(struct nlattr *data[],
				   struct ip_tunnel_encap *encap);

void ip_tunnel_netlink_parms(struct nlattr *data[],
			     struct ip_tunnel_parm_kern *parms);

extern const struct header_ops ip_tunnel_header_ops;
__be16 ip_tunnel_parse_protocol(const struct sk_buff *skb);

struct ip_tunnel_encap_ops {
	size_t (*encap_hlen)(struct ip_tunnel_encap *e);
	int (*build_header)(struct sk_buff *skb, struct ip_tunnel_encap *e,
			    u8 *protocol, struct flowi4 *fl4);
	int (*err_handler)(struct sk_buff *skb, u32 info);
};

#define MAX_IPTUN_ENCAP_OPS 8

extern const struct ip_tunnel_encap_ops __rcu *
		iptun_encaps[MAX_IPTUN_ENCAP_OPS];

int ip_tunnel_encap_add_ops(const struct ip_tunnel_encap_ops *op,
			    unsigned int num);
int ip_tunnel_encap_del_ops(const struct ip_tunnel_encap_ops *op,
			    unsigned int num);

int ip_tunnel_encap_setup(struct ip_tunnel *t,
			  struct ip_tunnel_encap *ipencap);

static inline enum skb_drop_reason
pskb_inet_may_pull_reason(struct sk_buff *skb)
{
	int nhlen;

	switch (skb->protocol) {
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		nhlen = sizeof(struct ipv6hdr);
		break;
#endif
	case htons(ETH_P_IP):
		nhlen = sizeof(struct iphdr);
		break;
	default:
		nhlen = 0;
	}

	return pskb_network_may_pull_reason(skb, nhlen);
}

static inline bool pskb_inet_may_pull(struct sk_buff *skb)
{
	return pskb_inet_may_pull_reason(skb) == SKB_NOT_DROPPED_YET;
}

/* Variant of pskb_inet_may_pull().
 */
static inline enum skb_drop_reason
skb_vlan_inet_prepare(struct sk_buff *skb, bool inner_proto_inherit)
{
	int nhlen = 0, maclen = inner_proto_inherit ? 0 : ETH_HLEN;
	__be16 type = skb->protocol;
	enum skb_drop_reason reason;

	/* Essentially this is skb_protocol(skb, true)
	 * And we get MAC len.
	 */
	if (eth_type_vlan(type))
		type = __vlan_get_protocol(skb, type, &maclen);

	switch (type) {
#if IS_ENABLED(CONFIG_IPV6)
	case htons(ETH_P_IPV6):
		nhlen = sizeof(struct ipv6hdr);
		break;
#endif
	case htons(ETH_P_IP):
		nhlen = sizeof(struct iphdr);
		break;
	}
	/* For ETH_P_IPV6/ETH_P_IP we make sure to pull
	 * a base network header in skb->head.
	 */
	reason = pskb_may_pull_reason(skb, maclen + nhlen);
	if (reason)
		return reason;

	skb_set_network_header(skb, maclen);

	return SKB_NOT_DROPPED_YET;
}

static inline int ip_encap_hlen(struct ip_tunnel_encap *e)
{
	const struct ip_tunnel_encap_ops *ops;
	int hlen = -EINVAL;

	if (e->type == TUNNEL_ENCAP_NONE)
		return 0;

	if (e->type >= MAX_IPTUN_ENCAP_OPS)
		return -EINVAL;

	rcu_read_lock();
	ops = rcu_dereference(iptun_encaps[e->type]);
	if (likely(ops && ops->encap_hlen))
		hlen = ops->encap_hlen(e);
	rcu_read_unlock();

	return hlen;
}

static inline int ip_tunnel_encap(struct sk_buff *skb,
				  struct ip_tunnel_encap *e,
				  u8 *protocol, struct flowi4 *fl4)
{
	const struct ip_tunnel_encap_ops *ops;
	int ret = -EINVAL;

	if (e->type == TUNNEL_ENCAP_NONE)
		return 0;

	if (e->type >= MAX_IPTUN_ENCAP_OPS)
		return -EINVAL;

	rcu_read_lock();
	ops = rcu_dereference(iptun_encaps[e->type]);
	if (likely(ops && ops->build_header))
		ret = ops->build_header(skb, e, protocol, fl4);
	rcu_read_unlock();

	return ret;
}

/* Extract dsfield from inner protocol */
static inline u8 ip_tunnel_get_dsfield(const struct iphdr *iph,
				       const struct sk_buff *skb)
{
	__be16 payload_protocol = skb_protocol(skb, true);

	if (payload_protocol == htons(ETH_P_IP))
		return iph->tos;
	else if (payload_protocol == htons(ETH_P_IPV6))
		return ipv6_get_dsfield((const struct ipv6hdr *)iph);
	else
		return 0;
}

static inline __be32 ip_tunnel_get_flowlabel(const struct iphdr *iph,
					     const struct sk_buff *skb)
{
	__be16 payload_protocol = skb_protocol(skb, true);

	if (payload_protocol == htons(ETH_P_IPV6))
		return ip6_flowlabel((const struct ipv6hdr *)iph);
	else
		return 0;
}

static inline u8 ip_tunnel_get_ttl(const struct iphdr *iph,
				       const struct sk_buff *skb)
{
	__be16 payload_protocol = skb_protocol(skb, true);

	if (payload_protocol == htons(ETH_P_IP))
		return iph->ttl;
	else if (payload_protocol == htons(ETH_P_IPV6))
		return ((const struct ipv6hdr *)iph)->hop_limit;
	else
		return 0;
}

/* Propagate ECN bits out */
static inline u8 ip_tunnel_ecn_encap(u8 tos, const struct iphdr *iph,
				     const struct sk_buff *skb)
{
	u8 inner = ip_tunnel_get_dsfield(iph, skb);

	return INET_ECN_encapsulate(tos, inner);
}

int __iptunnel_pull_header(struct sk_buff *skb, int hdr_len,
			   __be16 inner_proto, bool raw_proto, bool xnet);

static inline int iptunnel_pull_header(struct sk_buff *skb, int hdr_len,
				       __be16 inner_proto, bool xnet)
{
	return __iptunnel_pull_header(skb, hdr_len, inner_proto, false, xnet);
}

void iptunnel_xmit(struct sock *sk, struct rtable *rt, struct sk_buff *skb,
		   __be32 src, __be32 dst, u8 proto,
		   u8 tos, u8 ttl, __be16 df, bool xnet, u16 ipcb_flags);
struct metadata_dst *iptunnel_metadata_reply(struct metadata_dst *md,
					     gfp_t flags);
int skb_tunnel_check_pmtu(struct sk_buff *skb, struct dst_entry *encap_dst,
			  int headroom, bool reply);

int iptunnel_handle_offloads(struct sk_buff *skb, int gso_type_mask);

static inline int iptunnel_pull_offloads(struct sk_buff *skb)
{
	if (skb_is_gso(skb)) {
		int err;

		err = skb_unclone(skb, GFP_ATOMIC);
		if (unlikely(err))
			return err;
		skb_shinfo(skb)->gso_type &= ~(NETIF_F_GSO_ENCAP_ALL >>
					       NETIF_F_GSO_SHIFT);
	}

	skb->encapsulation = 0;
	return 0;
}

static inline void iptunnel_xmit_stats(struct net_device *dev, int pkt_len)
{
	if (pkt_len > 0) {
		struct pcpu_sw_netstats *tstats = get_cpu_ptr(dev->tstats);

		u64_stats_update_begin(&tstats->syncp);
		u64_stats_add(&tstats->tx_bytes, pkt_len);
		u64_stats_inc(&tstats->tx_packets);
		u64_stats_update_end(&tstats->syncp);
		put_cpu_ptr(tstats);
		return;
	}

	if (pkt_len < 0) {
		DEV_STATS_INC(dev, tx_errors);
		DEV_STATS_INC(dev, tx_aborted_errors);
	} else {
		DEV_STATS_INC(dev, tx_dropped);
	}
}

static inline void ip_tunnel_info_opts_get(void *to,
					   const struct ip_tunnel_info *info)
{
	memcpy(to, ip_tunnel_info_opts(info), info->options_len);
}

static inline void ip_tunnel_info_opts_set(struct ip_tunnel_info *info,
					   const void *from, int len,
					   const unsigned long *flags)
{
	info->options_len = len;
	if (len > 0) {
		memcpy(ip_tunnel_info_opts(info), from, len);
		ip_tunnel_flags_or(info->key.tun_flags, info->key.tun_flags,
				   flags);
	}
}

static inline struct ip_tunnel_info *lwt_tun_info(struct lwtunnel_state *lwtstate)
{
	return (struct ip_tunnel_info *)lwtstate->data;
}

DECLARE_STATIC_KEY_FALSE(ip_tunnel_metadata_cnt);

/* Returns > 0 if metadata should be collected */
static inline int ip_tunnel_collect_metadata(void)
{
	return static_branch_unlikely(&ip_tunnel_metadata_cnt);
}

void __init ip_tunnel_core_init(void);

void ip_tunnel_need_metadata(void);
void ip_tunnel_unneed_metadata(void);

#else /* CONFIG_INET */

static inline struct ip_tunnel_info *lwt_tun_info(struct lwtunnel_state *lwtstate)
{
	return NULL;
}

static inline void ip_tunnel_need_metadata(void)
{
}

static inline void ip_tunnel_unneed_metadata(void)
{
}

static inline void ip_tunnel_info_opts_get(void *to,
					   const struct ip_tunnel_info *info)
{
}

static inline void ip_tunnel_info_opts_set(struct ip_tunnel_info *info,
					   const void *from, int len,
					   const unsigned long *flags)
{
	info->options_len = 0;
}

#endif /* CONFIG_INET */

#endif /* __NET_IP_TUNNELS_H */
