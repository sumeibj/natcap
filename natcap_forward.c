/*
 * Author: Chen Minqiang <ptpt52@gmail.com>
 *  Date : Sun, 05 Jun 2016 16:23:40 +0800
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_ether.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_nat_core.h>
#include "natcap.h"
#include "natcap_common.h"
#include "natcap_forward.h"
#include "natcap_client.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
static unsigned int natcap_forward_pre_ct_in_hook(unsigned int hooknum,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0)
static unsigned int natcap_forward_pre_ct_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct net_device *in,
		const struct net_device *out,
		int (*okfn)(struct sk_buff *))
{
	unsigned int hooknum = ops->hooknum;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static unsigned int natcap_forward_pre_ct_in_hook(const struct nf_hook_ops *ops,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	unsigned int hooknum = state->hook;
#else
static unsigned int natcap_forward_pre_ct_in_hook(void *priv,
		struct sk_buff *skb,
		const struct nf_hook_state *state)
{
	unsigned int hooknum = state->hook;
#endif
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	struct iphdr *iph;
	void *l4;
	struct tuple server;

	if (disabled)
		return NF_ACCEPT;

	iph = ip_hdr(skb);
	if (iph->protocol != IPPROTO_TCP && iph->protocol != IPPROTO_UDP) {
		return NF_ACCEPT;
	}

	ct = nf_ct_get(skb, &ctinfo);
	if (NULL == ct) {
		return NF_ACCEPT;
	}
	if (test_bit(IPS_NATCAP_BYPASS_BIT, &ct->status)) {
		return NF_ACCEPT;
	}
	if (test_bit(IPS_NATCAP_BIT, &ct->status)) {
		skb->mark = XT_MARK_NATCAP;
		return NF_ACCEPT;
	}
	if (CTINFO2DIR(ctinfo) != IP_CT_DIR_ORIGINAL) {
		return NF_ACCEPT;
	}

	if (iph->protocol == IPPROTO_TCP) {
		if (!skb_make_writable(skb, iph->ihl * 4 + sizeof(struct tcphdr))) {
			return NF_DROP;
		}
		iph = ip_hdr(skb);
		l4 = (void *)iph + iph->ihl * 4;
		if (TCPH(l4)->doff * 4 < sizeof(struct tcphdr)) {
			return NF_DROP;
		}
		if (!skb_make_writable(skb, iph->ihl * 4 + TCPH(l4)->doff * 4)) {
			return NF_DROP;
		}
		iph = ip_hdr(skb);
		l4 = (void *)iph + iph->ihl * 4;

		if (!TCPH(l4)->syn || TCPH(l4)->ack) {
			NATCAP_INFO("(FPCI)" DEBUG_TCP_FMT ": first packet in but not syn\n", DEBUG_TCP_ARG(iph,l4));
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			return NF_ACCEPT;
		}

		if (natcap_tcp_decode_header(TCPH(l4)) == NULL) {
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			return NF_ACCEPT;
		}

		if (!test_and_set_bit(IPS_NATCAP_BIT, &ct->status)) { /* first time in */
			natcap_server_info_select(iph->daddr, TCPH(l4)->dest, &server);
			if (server.ip == 0) {
				NATCAP_DEBUG("(FPCI)" DEBUG_TCP_FMT ": no server found\n", DEBUG_TCP_ARG(iph,l4));
				set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
				return NF_ACCEPT;
			}
			NATCAP_INFO("(FPCI)" DEBUG_TCP_FMT ": new connection, after decode target=" TUPLE_FMT "\n", DEBUG_TCP_ARG(iph,l4), TUPLE_ARG(&server));
			if (natcap_dnat_setup(ct, server.ip, server.port) != NF_ACCEPT) {
				NATCAP_ERROR("(FPCI)" DEBUG_TCP_FMT ": natcap_dnat_setup failed, target=" TUPLE_FMT "\n", DEBUG_TCP_ARG(iph,l4), TUPLE_ARG(&server));
				set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
				return NF_DROP;
			}
		}

		skb->mark = XT_MARK_NATCAP;
		NATCAP_DEBUG("(FPCI)" DEBUG_TCP_FMT ": after decode\n", DEBUG_TCP_ARG(iph,l4));
	} else if (iph->protocol == IPPROTO_UDP) {
		if (!skb_make_writable(skb, iph->ihl * 4 + sizeof(struct udphdr))) {
			return NF_DROP;
		}
		iph = ip_hdr(skb);
		l4 = (void *)iph + iph->ihl * 4;

		if (skb_make_writable(skb, iph->ihl * 4 + sizeof(struct udphdr) + 12) &&
				*((unsigned int *)((void *)UDPH(l4) + sizeof(struct udphdr))) == __constant_htonl(0xFFFE0099)) {
			iph = ip_hdr(skb);
			l4 = (void *)iph + iph->ihl * 4;

			if (skb->ip_summed == CHECKSUM_NONE) {
				if (skb_rcsum_verify(skb) != 0) {
					NATCAP_WARN("(FPCI)" DEBUG_UDP_FMT ": skb_rcsum_verify fail\n", DEBUG_UDP_ARG(iph,l4));
					return NF_DROP;
				}
				skb->csum = 0;
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			}

			if (!test_and_set_bit(IPS_NATCAP_BIT, &ct->status)) { /* first time in */
				natcap_server_info_select(iph->daddr, UDPH(l4)->dest, &server);
				if (server.ip == 0) {
					NATCAP_DEBUG("(FPCI)" DEBUG_UDP_FMT ": no server found\n", DEBUG_UDP_ARG(iph,l4));
					set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
					return NF_ACCEPT;
				}
				NATCAP_INFO("(FPCI)" DEBUG_UDP_FMT ": new connection, after decode target=" TUPLE_FMT "\n", DEBUG_UDP_ARG(iph,l4), TUPLE_ARG(&server));
				if (natcap_dnat_setup(ct, server.ip, server.port) != NF_ACCEPT) {
					NATCAP_ERROR("(FPCI)" DEBUG_UDP_FMT ": natcap_dnat_setup failed, target=" TUPLE_FMT "\n", DEBUG_UDP_ARG(iph,l4), TUPLE_ARG(&server));
					set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
					return NF_DROP;
				}
			}

			NATCAP_INFO("(FPCI)" DEBUG_UDP_FMT ": pass ctrl decode\n", DEBUG_UDP_ARG(iph,l4));
		} else if (skb_make_writable(skb, iph->ihl * 4 + sizeof(struct tcphdr) + 8) &&
				*((unsigned int *)((void *)UDPH(l4) + 8)) == __constant_htonl(0xFFFF0099)) {
			iph = ip_hdr(skb);
			l4 = (void *)iph + iph->ihl * 4;
			if (!skb_make_writable(skb, iph->ihl * 4 + TCPH(l4 + 8)->doff * 4)) {
				return NF_DROP;
			}
			iph = ip_hdr(skb);
			l4 = (void *)iph + iph->ihl * 4;

			if (!TCPH(l4 + 8)->syn || TCPH(l4 + 8)->ack) {
				NATCAP_INFO("(FPCI)" DEBUG_UDP_FMT ": UDP first packet in but not syn\n", DEBUG_UDP_ARG(iph,l4));
				set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
				return NF_ACCEPT;
			}

			if (natcap_tcp_decode_header(TCPH(l4 + 8)) == NULL) {
				set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
				return NF_ACCEPT;
			}

			if (!test_and_set_bit(IPS_NATCAP_BIT, &ct->status)) { /* first time in */
				natcap_server_info_select(iph->daddr, UDPH(l4)->dest, &server);
				if (server.ip == 0) {
					NATCAP_DEBUG("(FPCI)" DEBUG_UDP_FMT ": no server found\n", DEBUG_UDP_ARG(iph,l4));
					set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
					return NF_ACCEPT;
				}
				NATCAP_INFO("(FPCI)" DEBUG_UDP_FMT ": new connection, after decode target=" TUPLE_FMT "\n", DEBUG_UDP_ARG(iph,l4), TUPLE_ARG(&server));
				if (natcap_dnat_setup(ct, server.ip, server.port) != NF_ACCEPT) {
					NATCAP_ERROR("(FPCI)" DEBUG_UDP_FMT ": natcap_dnat_setup failed, target=" TUPLE_FMT "\n", DEBUG_UDP_ARG(iph,l4), TUPLE_ARG(&server));
					set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
					return NF_DROP;
				}
			}

			NATCAP_INFO("(FPCI)" DEBUG_UDP_FMT ": pass UDP encoded data\n", DEBUG_UDP_ARG(iph,l4));
		}

		iph = ip_hdr(skb);
		l4 = (void *)iph + iph->ihl * 4;

		if (test_bit(IPS_NATCAP_BIT, &ct->status)) {
			skb->mark = XT_MARK_NATCAP;
		} else {
			set_bit(IPS_NATCAP_BYPASS_BIT, &ct->status);
			NATCAP_DEBUG("(FPCI)" DEBUG_UDP_FMT ": first packet in but not ctrl code\n", DEBUG_UDP_ARG(iph,l4));
		}
	}

	return NF_ACCEPT;
}

static struct nf_hook_ops forward_hooks[] = {
	{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
		.owner = THIS_MODULE,
#endif
		.hook = natcap_forward_pre_ct_in_hook,
		.pf = PF_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_NAT_DST - 35,
	},
};

int natcap_forward_init(void)
{
	int ret = 0;

	need_conntrack();

	natcap_server_info_cleanup();
	ret = nf_register_hooks(forward_hooks, ARRAY_SIZE(forward_hooks));
	return ret;
}

void natcap_forward_exit(void)
{
	nf_unregister_hooks(forward_hooks, ARRAY_SIZE(forward_hooks));
}
