/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/queue.h>

#include <rte_string_fns.h>
#include <rte_memzone.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_tcp.h>
#include <rte_sctp.h>
#include <rte_udp.h>
#include <rte_ip.h>
#include <rte_net.h>

#include "i40e_logs.h"
#include "base/i40e_prototype.h"
#include "base/i40e_type.h"
#include "i40e_ethdev.h"
#include "i40e_rxtx.h"

#define DEFAULT_TX_RS_THRESH   32
#define DEFAULT_TX_FREE_THRESH 32
#define I40E_MAX_PKT_TYPE      256

#define I40E_TX_MAX_BURST  32

#define I40E_DMA_MEM_ALIGN 4096

/* Base address of the HW descriptor ring should be 128B aligned. */
#define I40E_RING_BASE_ALIGN	128

#define I40E_SIMPLE_FLAGS ((uint32_t)ETH_TXQ_FLAGS_NOMULTSEGS | \
					ETH_TXQ_FLAGS_NOOFFLOADS)

#define I40E_TXD_CMD (I40E_TX_DESC_CMD_EOP | I40E_TX_DESC_CMD_RS)

#ifdef RTE_LIBRTE_IEEE1588
#define I40E_TX_IEEE1588_TMST PKT_TX_IEEE1588_TMST
#else
#define I40E_TX_IEEE1588_TMST 0
#endif

#define I40E_TX_CKSUM_OFFLOAD_MASK (		 \
		PKT_TX_IP_CKSUM |		 \
		PKT_TX_L4_MASK |		 \
		PKT_TX_TCP_SEG |		 \
		PKT_TX_OUTER_IP_CKSUM)

#define I40E_TX_OFFLOAD_MASK (  \
		PKT_TX_IP_CKSUM |       \
		PKT_TX_L4_MASK |        \
		PKT_TX_OUTER_IP_CKSUM | \
		PKT_TX_TCP_SEG |        \
		PKT_TX_QINQ_PKT |       \
		PKT_TX_VLAN_PKT |	\
		PKT_TX_TUNNEL_MASK |	\
		I40E_TX_IEEE1588_TMST)

#define I40E_TX_OFFLOAD_NOTSUP_MASK \
		(PKT_TX_OFFLOAD_MASK ^ I40E_TX_OFFLOAD_MASK)

static uint16_t i40e_xmit_pkts_simple(void *tx_queue,
				      struct rte_mbuf **tx_pkts,
				      uint16_t nb_pkts);

static inline void
i40e_rxd_to_vlan_tci(struct rte_mbuf *mb, volatile union i40e_rx_desc *rxdp)
{
	if (rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len) &
		(1 << I40E_RX_DESC_STATUS_L2TAG1P_SHIFT)) {
		mb->ol_flags |= PKT_RX_VLAN_PKT | PKT_RX_VLAN_STRIPPED;
		mb->vlan_tci =
			rte_le_to_cpu_16(rxdp->wb.qword0.lo_dword.l2tag1);
		PMD_RX_LOG(DEBUG, "Descriptor l2tag1: %u",
			   rte_le_to_cpu_16(rxdp->wb.qword0.lo_dword.l2tag1));
	} else {
		mb->vlan_tci = 0;
	}
#ifndef RTE_LIBRTE_I40E_16BYTE_RX_DESC
	if (rte_le_to_cpu_16(rxdp->wb.qword2.ext_status) &
		(1 << I40E_RX_DESC_EXT_STATUS_L2TAG2P_SHIFT)) {
		mb->ol_flags |= PKT_RX_QINQ_STRIPPED;
		mb->vlan_tci_outer = mb->vlan_tci;
		mb->vlan_tci = rte_le_to_cpu_16(rxdp->wb.qword2.l2tag2_2);
		PMD_RX_LOG(DEBUG, "Descriptor l2tag2_1: %u, l2tag2_2: %u",
			   rte_le_to_cpu_16(rxdp->wb.qword2.l2tag2_1),
			   rte_le_to_cpu_16(rxdp->wb.qword2.l2tag2_2));
	} else {
		mb->vlan_tci_outer = 0;
	}
#endif
	PMD_RX_LOG(DEBUG, "Mbuf vlan_tci: %u, vlan_tci_outer: %u",
		   mb->vlan_tci, mb->vlan_tci_outer);
}

/* Translate the rx descriptor status to pkt flags */
static inline uint64_t
i40e_rxd_status_to_pkt_flags(uint64_t qword)
{
	uint64_t flags;

	/* Check if RSS_HASH */
	flags = (((qword >> I40E_RX_DESC_STATUS_FLTSTAT_SHIFT) &
					I40E_RX_DESC_FLTSTAT_RSS_HASH) ==
			I40E_RX_DESC_FLTSTAT_RSS_HASH) ? PKT_RX_RSS_HASH : 0;

	/* Check if FDIR Match */
	flags |= (qword & (1 << I40E_RX_DESC_STATUS_FLM_SHIFT) ?
							PKT_RX_FDIR : 0);

	return flags;
}

static inline uint64_t
i40e_rxd_error_to_pkt_flags(uint64_t qword)
{
	uint64_t flags = 0;
	uint64_t error_bits = (qword >> I40E_RXD_QW1_ERROR_SHIFT);

#define I40E_RX_ERR_BITS 0x3f
	if (likely((error_bits & I40E_RX_ERR_BITS) == 0)) {
		flags |= (PKT_RX_IP_CKSUM_GOOD | PKT_RX_L4_CKSUM_GOOD);
		return flags;
	}

	if (unlikely(error_bits & (1 << I40E_RX_DESC_ERROR_IPE_SHIFT)))
		flags |= PKT_RX_IP_CKSUM_BAD;
	else
		flags |= PKT_RX_IP_CKSUM_GOOD;

	if (unlikely(error_bits & (1 << I40E_RX_DESC_ERROR_L4E_SHIFT)))
		flags |= PKT_RX_L4_CKSUM_BAD;
	else
		flags |= PKT_RX_L4_CKSUM_GOOD;

	if (unlikely(error_bits & (1 << I40E_RX_DESC_ERROR_EIPE_SHIFT)))
		flags |= PKT_RX_EIP_CKSUM_BAD;

	return flags;
}

/* Function to check and set the ieee1588 timesync index and get the
 * appropriate flags.
 */
#ifdef RTE_LIBRTE_IEEE1588
static inline uint64_t
i40e_get_iee15888_flags(struct rte_mbuf *mb, uint64_t qword)
{
	uint64_t pkt_flags = 0;
	uint16_t tsyn = (qword & (I40E_RXD_QW1_STATUS_TSYNVALID_MASK
				  | I40E_RXD_QW1_STATUS_TSYNINDX_MASK))
				    >> I40E_RX_DESC_STATUS_TSYNINDX_SHIFT;

	if ((mb->packet_type & RTE_PTYPE_L2_MASK)
			== RTE_PTYPE_L2_ETHER_TIMESYNC)
		pkt_flags = PKT_RX_IEEE1588_PTP;
	if (tsyn & 0x04) {
		pkt_flags |= PKT_RX_IEEE1588_TMST;
		mb->timesync = tsyn & 0x03;
	}

	return pkt_flags;
}
#endif

#define I40E_RX_DESC_EXT_STATUS_FLEXBH_MASK   0x03
#define I40E_RX_DESC_EXT_STATUS_FLEXBH_FD_ID  0x01
#define I40E_RX_DESC_EXT_STATUS_FLEXBH_FLEX   0x02
#define I40E_RX_DESC_EXT_STATUS_FLEXBL_MASK   0x03
#define I40E_RX_DESC_EXT_STATUS_FLEXBL_FLEX   0x01

static inline uint64_t
i40e_rxd_build_fdir(volatile union i40e_rx_desc *rxdp, struct rte_mbuf *mb)
{
	uint64_t flags = 0;
#ifndef RTE_LIBRTE_I40E_16BYTE_RX_DESC
	uint16_t flexbh, flexbl;

	flexbh = (rte_le_to_cpu_32(rxdp->wb.qword2.ext_status) >>
		I40E_RX_DESC_EXT_STATUS_FLEXBH_SHIFT) &
		I40E_RX_DESC_EXT_STATUS_FLEXBH_MASK;
	flexbl = (rte_le_to_cpu_32(rxdp->wb.qword2.ext_status) >>
		I40E_RX_DESC_EXT_STATUS_FLEXBL_SHIFT) &
		I40E_RX_DESC_EXT_STATUS_FLEXBL_MASK;


	if (flexbh == I40E_RX_DESC_EXT_STATUS_FLEXBH_FD_ID) {
		mb->hash.fdir.hi =
			rte_le_to_cpu_32(rxdp->wb.qword3.hi_dword.fd_id);
		flags |= PKT_RX_FDIR_ID;
	} else if (flexbh == I40E_RX_DESC_EXT_STATUS_FLEXBH_FLEX) {
		mb->hash.fdir.hi =
			rte_le_to_cpu_32(rxdp->wb.qword3.hi_dword.flex_bytes_hi);
		flags |= PKT_RX_FDIR_FLX;
	}
	if (flexbl == I40E_RX_DESC_EXT_STATUS_FLEXBL_FLEX) {
		mb->hash.fdir.lo =
			rte_le_to_cpu_32(rxdp->wb.qword3.lo_dword.flex_bytes_lo);
		flags |= PKT_RX_FDIR_FLX;
	}
#else
	mb->hash.fdir.hi =
		rte_le_to_cpu_32(rxdp->wb.qword0.hi_dword.fd_id);
	flags |= PKT_RX_FDIR_ID;
#endif
	return flags;
}

static inline void
i40e_parse_tunneling_params(uint64_t ol_flags,
			    union i40e_tx_offload tx_offload,
			    uint32_t *cd_tunneling)
{
	/* EIPT: External (outer) IP header type */
	if (ol_flags & PKT_TX_OUTER_IP_CKSUM)
		*cd_tunneling |= I40E_TX_CTX_EXT_IP_IPV4;
	else if (ol_flags & PKT_TX_OUTER_IPV4)
		*cd_tunneling |= I40E_TX_CTX_EXT_IP_IPV4_NO_CSUM;
	else if (ol_flags & PKT_TX_OUTER_IPV6)
		*cd_tunneling |= I40E_TX_CTX_EXT_IP_IPV6;

	/* EIPLEN: External (outer) IP header length, in DWords */
	*cd_tunneling |= (tx_offload.outer_l3_len >> 2) <<
		I40E_TXD_CTX_QW0_EXT_IPLEN_SHIFT;

	/* L4TUNT: L4 Tunneling Type */
	switch (ol_flags & PKT_TX_TUNNEL_MASK) {
	case PKT_TX_TUNNEL_IPIP:
		/* for non UDP / GRE tunneling, set to 00b */
		break;
	case PKT_TX_TUNNEL_VXLAN:
	case PKT_TX_TUNNEL_GENEVE:
		*cd_tunneling |= I40E_TXD_CTX_UDP_TUNNELING;
		break;
	case PKT_TX_TUNNEL_GRE:
		*cd_tunneling |= I40E_TXD_CTX_GRE_TUNNELING;
		break;
	default:
		PMD_TX_LOG(ERR, "Tunnel type not supported");
		return;
	}

	/* L4TUNLEN: L4 Tunneling Length, in Words
	 *
	 * We depend on app to set rte_mbuf.l2_len correctly.
	 * For IP in GRE it should be set to the length of the GRE
	 * header;
	 * for MAC in GRE or MAC in UDP it should be set to the length
	 * of the GRE or UDP headers plus the inner MAC up to including
	 * its last Ethertype.
	 */
	*cd_tunneling |= (tx_offload.l2_len >> 1) <<
		I40E_TXD_CTX_QW0_NATLEN_SHIFT;
}

static inline void
i40e_txd_enable_checksum(uint64_t ol_flags,
			uint32_t *td_cmd,
			uint32_t *td_offset,
			union i40e_tx_offload tx_offload)
{
	/* Set MACLEN */
	if (ol_flags & PKT_TX_TUNNEL_MASK)
		*td_offset |= (tx_offload.outer_l2_len >> 1)
				<< I40E_TX_DESC_LENGTH_MACLEN_SHIFT;
	else
		*td_offset |= (tx_offload.l2_len >> 1)
			<< I40E_TX_DESC_LENGTH_MACLEN_SHIFT;

	/* Enable L3 checksum offloads */
	if (ol_flags & PKT_TX_IP_CKSUM) {
		*td_cmd |= I40E_TX_DESC_CMD_IIPT_IPV4_CSUM;
		*td_offset |= (tx_offload.l3_len >> 2)
				<< I40E_TX_DESC_LENGTH_IPLEN_SHIFT;
	} else if (ol_flags & PKT_TX_IPV4) {
		*td_cmd |= I40E_TX_DESC_CMD_IIPT_IPV4;
		*td_offset |= (tx_offload.l3_len >> 2)
				<< I40E_TX_DESC_LENGTH_IPLEN_SHIFT;
	} else if (ol_flags & PKT_TX_IPV6) {
		*td_cmd |= I40E_TX_DESC_CMD_IIPT_IPV6;
		*td_offset |= (tx_offload.l3_len >> 2)
				<< I40E_TX_DESC_LENGTH_IPLEN_SHIFT;
	}

	if (ol_flags & PKT_TX_TCP_SEG) {
		*td_cmd |= I40E_TX_DESC_CMD_L4T_EOFT_TCP;
		*td_offset |= (tx_offload.l4_len >> 2)
			<< I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		return;
	}

	/* Enable L4 checksum offloads */
	switch (ol_flags & PKT_TX_L4_MASK) {
	case PKT_TX_TCP_CKSUM:
		*td_cmd |= I40E_TX_DESC_CMD_L4T_EOFT_TCP;
		*td_offset |= (sizeof(struct tcp_hdr) >> 2) <<
				I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	case PKT_TX_SCTP_CKSUM:
		*td_cmd |= I40E_TX_DESC_CMD_L4T_EOFT_SCTP;
		*td_offset |= (sizeof(struct sctp_hdr) >> 2) <<
				I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	case PKT_TX_UDP_CKSUM:
		*td_cmd |= I40E_TX_DESC_CMD_L4T_EOFT_UDP;
		*td_offset |= (sizeof(struct udp_hdr) >> 2) <<
				I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	default:
		break;
	}
}

/* Construct the tx flags */
static inline uint64_t
i40e_build_ctob(uint32_t td_cmd,
		uint32_t td_offset,
		unsigned int size,
		uint32_t td_tag)
{
	return rte_cpu_to_le_64(I40E_TX_DESC_DTYPE_DATA |
			((uint64_t)td_cmd  << I40E_TXD_QW1_CMD_SHIFT) |
			((uint64_t)td_offset << I40E_TXD_QW1_OFFSET_SHIFT) |
			((uint64_t)size  << I40E_TXD_QW1_TX_BUF_SZ_SHIFT) |
			((uint64_t)td_tag  << I40E_TXD_QW1_L2TAG1_SHIFT));
}

static inline int
i40e_xmit_cleanup(struct i40e_tx_queue *txq)
{
	struct i40e_tx_entry *sw_ring = txq->sw_ring;
	volatile struct i40e_tx_desc *txd = txq->tx_ring;
	uint16_t last_desc_cleaned = txq->last_desc_cleaned;
	uint16_t nb_tx_desc = txq->nb_tx_desc;
	uint16_t desc_to_clean_to;
	uint16_t nb_tx_to_clean;

	desc_to_clean_to = (uint16_t)(last_desc_cleaned + txq->tx_rs_thresh);
	if (desc_to_clean_to >= nb_tx_desc)
		desc_to_clean_to = (uint16_t)(desc_to_clean_to - nb_tx_desc);

	desc_to_clean_to = sw_ring[desc_to_clean_to].last_id;
	if ((txd[desc_to_clean_to].cmd_type_offset_bsz &
			rte_cpu_to_le_64(I40E_TXD_QW1_DTYPE_MASK)) !=
			rte_cpu_to_le_64(I40E_TX_DESC_DTYPE_DESC_DONE)) {
		PMD_TX_FREE_LOG(DEBUG, "TX descriptor %4u is not done "
			"(port=%d queue=%d)", desc_to_clean_to,
				txq->port_id, txq->queue_id);
		return -1;
	}

	if (last_desc_cleaned > desc_to_clean_to)
		nb_tx_to_clean = (uint16_t)((nb_tx_desc - last_desc_cleaned) +
							desc_to_clean_to);
	else
		nb_tx_to_clean = (uint16_t)(desc_to_clean_to -
					last_desc_cleaned);

	txd[desc_to_clean_to].cmd_type_offset_bsz = 0;

	txq->last_desc_cleaned = desc_to_clean_to;
	txq->nb_tx_free = (uint16_t)(txq->nb_tx_free + nb_tx_to_clean);

	return 0;
}

static inline int
#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
check_rx_burst_bulk_alloc_preconditions(struct i40e_rx_queue *rxq)
#else
check_rx_burst_bulk_alloc_preconditions(__rte_unused struct i40e_rx_queue *rxq)
#endif
{
	int ret = 0;

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	if (!(rxq->rx_free_thresh >= RTE_PMD_I40E_RX_MAX_BURST)) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions: "
			     "rxq->rx_free_thresh=%d, "
			     "RTE_PMD_I40E_RX_MAX_BURST=%d",
			     rxq->rx_free_thresh, RTE_PMD_I40E_RX_MAX_BURST);
		ret = -EINVAL;
	} else if (!(rxq->rx_free_thresh < rxq->nb_rx_desc)) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions: "
			     "rxq->rx_free_thresh=%d, "
			     "rxq->nb_rx_desc=%d",
			     rxq->rx_free_thresh, rxq->nb_rx_desc);
		ret = -EINVAL;
	} else if (rxq->nb_rx_desc % rxq->rx_free_thresh != 0) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions: "
			     "rxq->nb_rx_desc=%d, "
			     "rxq->rx_free_thresh=%d",
			     rxq->nb_rx_desc, rxq->rx_free_thresh);
		ret = -EINVAL;
	}
#else
	ret = -EINVAL;
#endif

	return ret;
}

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
#define I40E_LOOK_AHEAD 8
#if (I40E_LOOK_AHEAD != 8)
#error "PMD I40E: I40E_LOOK_AHEAD must be 8\n"
#endif
static inline int
i40e_rx_scan_hw_ring(struct i40e_rx_queue *rxq)
{
	volatile union i40e_rx_desc *rxdp;
	struct i40e_rx_entry *rxep;
	struct rte_mbuf *mb;
	uint16_t pkt_len;
	uint64_t qword1;
	uint32_t rx_status;
	int32_t s[I40E_LOOK_AHEAD], nb_dd;
	int32_t i, j, nb_rx = 0;
	uint64_t pkt_flags;

	rxdp = &rxq->rx_ring[rxq->rx_tail];
	rxep = &rxq->sw_ring[rxq->rx_tail];

	qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
	rx_status = (qword1 & I40E_RXD_QW1_STATUS_MASK) >>
				I40E_RXD_QW1_STATUS_SHIFT;

	/* Make sure there is at least 1 packet to receive */
	if (!(rx_status & (1 << I40E_RX_DESC_STATUS_DD_SHIFT)))
		return 0;

	/**
	 * Scan LOOK_AHEAD descriptors at a time to determine which
	 * descriptors reference packets that are ready to be received.
	 */
	for (i = 0; i < RTE_PMD_I40E_RX_MAX_BURST; i+=I40E_LOOK_AHEAD,
			rxdp += I40E_LOOK_AHEAD, rxep += I40E_LOOK_AHEAD) {
		/* Read desc statuses backwards to avoid race condition */
		for (j = I40E_LOOK_AHEAD - 1; j >= 0; j--) {
			qword1 = rte_le_to_cpu_64(\
				rxdp[j].wb.qword1.status_error_len);
			s[j] = (qword1 & I40E_RXD_QW1_STATUS_MASK) >>
					I40E_RXD_QW1_STATUS_SHIFT;
		}

		rte_smp_rmb();

		/* Compute how many status bits were set */
		for (j = 0, nb_dd = 0; j < I40E_LOOK_AHEAD; j++)
			nb_dd += s[j] & (1 << I40E_RX_DESC_STATUS_DD_SHIFT);

		nb_rx += nb_dd;

		/* Translate descriptor info to mbuf parameters */
		for (j = 0; j < nb_dd; j++) {
			mb = rxep[j].mbuf;
			qword1 = rte_le_to_cpu_64(\
				rxdp[j].wb.qword1.status_error_len);
			pkt_len = ((qword1 & I40E_RXD_QW1_LENGTH_PBUF_MASK) >>
				I40E_RXD_QW1_LENGTH_PBUF_SHIFT) - rxq->crc_len;
			mb->data_len = pkt_len;
			mb->pkt_len = pkt_len;
			mb->ol_flags = 0;
			i40e_rxd_to_vlan_tci(mb, &rxdp[j]);
			pkt_flags = i40e_rxd_status_to_pkt_flags(qword1);
			pkt_flags |= i40e_rxd_error_to_pkt_flags(qword1);
			mb->packet_type =
				i40e_rxd_pkt_type_mapping((uint8_t)((qword1 &
						I40E_RXD_QW1_PTYPE_MASK) >>
						I40E_RXD_QW1_PTYPE_SHIFT));
			if (pkt_flags & PKT_RX_RSS_HASH)
				mb->hash.rss = rte_le_to_cpu_32(\
					rxdp[j].wb.qword0.hi_dword.rss);
			if (pkt_flags & PKT_RX_FDIR)
				pkt_flags |= i40e_rxd_build_fdir(&rxdp[j], mb);

#ifdef RTE_LIBRTE_IEEE1588
			pkt_flags |= i40e_get_iee15888_flags(mb, qword1);
#endif
			mb->ol_flags |= pkt_flags;

		}

		for (j = 0; j < I40E_LOOK_AHEAD; j++)
			rxq->rx_stage[i + j] = rxep[j].mbuf;

		if (nb_dd != I40E_LOOK_AHEAD)
			break;
	}

	/* Clear software ring entries */
	for (i = 0; i < nb_rx; i++)
		rxq->sw_ring[rxq->rx_tail + i].mbuf = NULL;

	return nb_rx;
}

static inline uint16_t
i40e_rx_fill_from_stage(struct i40e_rx_queue *rxq,
			struct rte_mbuf **rx_pkts,
			uint16_t nb_pkts)
{
	uint16_t i;
	struct rte_mbuf **stage = &rxq->rx_stage[rxq->rx_next_avail];

	nb_pkts = (uint16_t)RTE_MIN(nb_pkts, rxq->rx_nb_avail);

	for (i = 0; i < nb_pkts; i++)
		rx_pkts[i] = stage[i];

	rxq->rx_nb_avail = (uint16_t)(rxq->rx_nb_avail - nb_pkts);
	rxq->rx_next_avail = (uint16_t)(rxq->rx_next_avail + nb_pkts);

	return nb_pkts;
}

static inline int
i40e_rx_alloc_bufs(struct i40e_rx_queue *rxq)
{
	volatile union i40e_rx_desc *rxdp;
	struct i40e_rx_entry *rxep;
	struct rte_mbuf *mb;
	uint16_t alloc_idx, i;
	uint64_t dma_addr;
	int diag;

	/* Allocate buffers in bulk */
	alloc_idx = (uint16_t)(rxq->rx_free_trigger -
				(rxq->rx_free_thresh - 1));
	rxep = &(rxq->sw_ring[alloc_idx]);
	diag = rte_mempool_get_bulk(rxq->mp, (void *)rxep,
					rxq->rx_free_thresh);
	if (unlikely(diag != 0)) {
		PMD_DRV_LOG(ERR, "Failed to get mbufs in bulk");
		return -ENOMEM;
	}

	rxdp = &rxq->rx_ring[alloc_idx];
	for (i = 0; i < rxq->rx_free_thresh; i++) {
		if (likely(i < (rxq->rx_free_thresh - 1)))
			/* Prefetch next mbuf */
			rte_prefetch0(rxep[i + 1].mbuf);

		mb = rxep[i].mbuf;
		rte_mbuf_refcnt_set(mb, 1);
		mb->next = NULL;
		mb->data_off = RTE_PKTMBUF_HEADROOM;
		mb->nb_segs = 1;
		mb->port = rxq->port_id;
		dma_addr = rte_cpu_to_le_64(\
			rte_mbuf_data_dma_addr_default(mb));
		rxdp[i].read.hdr_addr = 0;
		rxdp[i].read.pkt_addr = dma_addr;
	}

	/* Update rx tail regsiter */
	rte_wmb();
	I40E_PCI_REG_WRITE_RELAXED(rxq->qrx_tail, rxq->rx_free_trigger);

	rxq->rx_free_trigger =
		(uint16_t)(rxq->rx_free_trigger + rxq->rx_free_thresh);
	if (rxq->rx_free_trigger >= rxq->nb_rx_desc)
		rxq->rx_free_trigger = (uint16_t)(rxq->rx_free_thresh - 1);

	return 0;
}

static inline uint16_t
rx_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct i40e_rx_queue *rxq = (struct i40e_rx_queue *)rx_queue;
	uint16_t nb_rx = 0;

	if (!nb_pkts)
		return 0;

	if (rxq->rx_nb_avail)
		return i40e_rx_fill_from_stage(rxq, rx_pkts, nb_pkts);

	nb_rx = (uint16_t)i40e_rx_scan_hw_ring(rxq);
	rxq->rx_next_avail = 0;
	rxq->rx_nb_avail = nb_rx;
	rxq->rx_tail = (uint16_t)(rxq->rx_tail + nb_rx);

	if (rxq->rx_tail > rxq->rx_free_trigger) {
		if (i40e_rx_alloc_bufs(rxq) != 0) {
			uint16_t i, j;

			PMD_RX_LOG(DEBUG, "Rx mbuf alloc failed for "
				   "port_id=%u, queue_id=%u",
				   rxq->port_id, rxq->queue_id);
			rxq->rx_nb_avail = 0;
			rxq->rx_tail = (uint16_t)(rxq->rx_tail - nb_rx);
			for (i = 0, j = rxq->rx_tail; i < nb_rx; i++, j++)
				rxq->sw_ring[j].mbuf = rxq->rx_stage[i];

			return 0;
		}
	}

	if (rxq->rx_tail >= rxq->nb_rx_desc)
		rxq->rx_tail = 0;

	if (rxq->rx_nb_avail)
		return i40e_rx_fill_from_stage(rxq, rx_pkts, nb_pkts);

	return 0;
}

static uint16_t
i40e_recv_pkts_bulk_alloc(void *rx_queue,
			  struct rte_mbuf **rx_pkts,
			  uint16_t nb_pkts)
{
	uint16_t nb_rx = 0, n, count;

	if (unlikely(nb_pkts == 0))
		return 0;

	if (likely(nb_pkts <= RTE_PMD_I40E_RX_MAX_BURST))
		return rx_recv_pkts(rx_queue, rx_pkts, nb_pkts);

	while (nb_pkts) {
		n = RTE_MIN(nb_pkts, RTE_PMD_I40E_RX_MAX_BURST);
		count = rx_recv_pkts(rx_queue, &rx_pkts[nb_rx], n);
		nb_rx = (uint16_t)(nb_rx + count);
		nb_pkts = (uint16_t)(nb_pkts - count);
		if (count < n)
			break;
	}

	return nb_rx;
}
#else
static uint16_t
i40e_recv_pkts_bulk_alloc(void __rte_unused *rx_queue,
			  struct rte_mbuf __rte_unused **rx_pkts,
			  uint16_t __rte_unused nb_pkts)
{
	return 0;
}
#endif /* RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC */

uint16_t
i40e_recv_pkts(void *rx_queue, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct i40e_rx_queue *rxq;
	volatile union i40e_rx_desc *rx_ring;
	volatile union i40e_rx_desc *rxdp;
	union i40e_rx_desc rxd;
	struct i40e_rx_entry *sw_ring;
	struct i40e_rx_entry *rxe;
	struct rte_mbuf *rxm;
	struct rte_mbuf *nmb;
	uint16_t nb_rx;
	uint32_t rx_status;
	uint64_t qword1;
	uint16_t rx_packet_len;
	uint16_t rx_id, nb_hold;
	uint64_t dma_addr;
	uint64_t pkt_flags;

	nb_rx = 0;
	nb_hold = 0;
	rxq = rx_queue;
	rx_id = rxq->rx_tail;
	rx_ring = rxq->rx_ring;
	sw_ring = rxq->sw_ring;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
		rx_status = (qword1 & I40E_RXD_QW1_STATUS_MASK)
				>> I40E_RXD_QW1_STATUS_SHIFT;

		/* Check the DD bit first */
		if (!(rx_status & (1 << I40E_RX_DESC_STATUS_DD_SHIFT)))
			break;

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb))
			break;
		rxd = *rxdp;

		nb_hold++;
		rxe = &sw_ring[rx_id];
		rx_id++;
		if (unlikely(rx_id == rxq->nb_rx_desc))
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(sw_ring[rx_id].mbuf);

		/**
		 * When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(&sw_ring[rx_id]);
		}
		rxm = rxe->mbuf;
		rxe->mbuf = nmb;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_dma_addr_default(nmb));
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;

		rx_packet_len = ((qword1 & I40E_RXD_QW1_LENGTH_PBUF_MASK) >>
				I40E_RXD_QW1_LENGTH_PBUF_SHIFT) - rxq->crc_len;

		rxm->data_off = RTE_PKTMBUF_HEADROOM;
		rte_prefetch0(RTE_PTR_ADD(rxm->buf_addr, RTE_PKTMBUF_HEADROOM));
		rxm->nb_segs = 1;
		rxm->next = NULL;
		rxm->pkt_len = rx_packet_len;
		rxm->data_len = rx_packet_len;
		rxm->port = rxq->port_id;
		rxm->ol_flags = 0;
		i40e_rxd_to_vlan_tci(rxm, &rxd);
		pkt_flags = i40e_rxd_status_to_pkt_flags(qword1);
		pkt_flags |= i40e_rxd_error_to_pkt_flags(qword1);
		rxm->packet_type =
			i40e_rxd_pkt_type_mapping((uint8_t)((qword1 &
			I40E_RXD_QW1_PTYPE_MASK) >> I40E_RXD_QW1_PTYPE_SHIFT));
		if (pkt_flags & PKT_RX_RSS_HASH)
			rxm->hash.rss =
				rte_le_to_cpu_32(rxd.wb.qword0.hi_dword.rss);
		if (pkt_flags & PKT_RX_FDIR)
			pkt_flags |= i40e_rxd_build_fdir(&rxd, rxm);

#ifdef RTE_LIBRTE_IEEE1588
		pkt_flags |= i40e_get_iee15888_flags(rxm, qword1);
#endif
		rxm->ol_flags |= pkt_flags;

		rx_pkts[nb_rx++] = rxm;
	}
	rxq->rx_tail = rx_id;

	/**
	 * If the number of free RX descriptors is greater than the RX free
	 * threshold of the queue, advance the receive tail register of queue.
	 * Update that register with the value of the last processed RX
	 * descriptor minus 1.
	 */
	nb_hold = (uint16_t)(nb_hold + rxq->nb_rx_hold);
	if (nb_hold > rxq->rx_free_thresh) {
		rx_id = (uint16_t) ((rx_id == 0) ?
			(rxq->nb_rx_desc - 1) : (rx_id - 1));
		I40E_PCI_REG_WRITE(rxq->qrx_tail, rx_id);
		nb_hold = 0;
	}
	rxq->nb_rx_hold = nb_hold;

	return nb_rx;
}

uint16_t
i40e_recv_scattered_pkts(void *rx_queue,
			 struct rte_mbuf **rx_pkts,
			 uint16_t nb_pkts)
{
	struct i40e_rx_queue *rxq = rx_queue;
	volatile union i40e_rx_desc *rx_ring = rxq->rx_ring;
	volatile union i40e_rx_desc *rxdp;
	union i40e_rx_desc rxd;
	struct i40e_rx_entry *sw_ring = rxq->sw_ring;
	struct i40e_rx_entry *rxe;
	struct rte_mbuf *first_seg = rxq->pkt_first_seg;
	struct rte_mbuf *last_seg = rxq->pkt_last_seg;
	struct rte_mbuf *nmb, *rxm;
	uint16_t rx_id = rxq->rx_tail;
	uint16_t nb_rx = 0, nb_hold = 0, rx_packet_len;
	uint32_t rx_status;
	uint64_t qword1;
	uint64_t dma_addr;
	uint64_t pkt_flags;

	while (nb_rx < nb_pkts) {
		rxdp = &rx_ring[rx_id];
		qword1 = rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len);
		rx_status = (qword1 & I40E_RXD_QW1_STATUS_MASK) >>
					I40E_RXD_QW1_STATUS_SHIFT;

		/* Check the DD bit */
		if (!(rx_status & (1 << I40E_RX_DESC_STATUS_DD_SHIFT)))
			break;

		nmb = rte_mbuf_raw_alloc(rxq->mp);
		if (unlikely(!nmb))
			break;
		rxd = *rxdp;
		nb_hold++;
		rxe = &sw_ring[rx_id];
		rx_id++;
		if (rx_id == rxq->nb_rx_desc)
			rx_id = 0;

		/* Prefetch next mbuf */
		rte_prefetch0(sw_ring[rx_id].mbuf);

		/**
		 * When next RX descriptor is on a cache line boundary,
		 * prefetch the next 4 RX descriptors and next 8 pointers
		 * to mbufs.
		 */
		if ((rx_id & 0x3) == 0) {
			rte_prefetch0(&rx_ring[rx_id]);
			rte_prefetch0(&sw_ring[rx_id]);
		}

		rxm = rxe->mbuf;
		rxe->mbuf = nmb;
		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_dma_addr_default(nmb));

		/* Set data buffer address and data length of the mbuf */
		rxdp->read.hdr_addr = 0;
		rxdp->read.pkt_addr = dma_addr;
		rx_packet_len = (qword1 & I40E_RXD_QW1_LENGTH_PBUF_MASK) >>
					I40E_RXD_QW1_LENGTH_PBUF_SHIFT;
		rxm->data_len = rx_packet_len;
		rxm->data_off = RTE_PKTMBUF_HEADROOM;

		/**
		 * If this is the first buffer of the received packet, set the
		 * pointer to the first mbuf of the packet and initialize its
		 * context. Otherwise, update the total length and the number
		 * of segments of the current scattered packet, and update the
		 * pointer to the last mbuf of the current packet.
		 */
		if (!first_seg) {
			first_seg = rxm;
			first_seg->nb_segs = 1;
			first_seg->pkt_len = rx_packet_len;
		} else {
			first_seg->pkt_len =
				(uint16_t)(first_seg->pkt_len +
						rx_packet_len);
			first_seg->nb_segs++;
			last_seg->next = rxm;
		}

		/**
		 * If this is not the last buffer of the received packet,
		 * update the pointer to the last mbuf of the current scattered
		 * packet and continue to parse the RX ring.
		 */
		if (!(rx_status & (1 << I40E_RX_DESC_STATUS_EOF_SHIFT))) {
			last_seg = rxm;
			continue;
		}

		/**
		 * This is the last buffer of the received packet. If the CRC
		 * is not stripped by the hardware:
		 *  - Subtract the CRC length from the total packet length.
		 *  - If the last buffer only contains the whole CRC or a part
		 *  of it, free the mbuf associated to the last buffer. If part
		 *  of the CRC is also contained in the previous mbuf, subtract
		 *  the length of that CRC part from the data length of the
		 *  previous mbuf.
		 */
		rxm->next = NULL;
		if (unlikely(rxq->crc_len > 0)) {
			first_seg->pkt_len -= ETHER_CRC_LEN;
			if (rx_packet_len <= ETHER_CRC_LEN) {
				rte_pktmbuf_free_seg(rxm);
				first_seg->nb_segs--;
				last_seg->data_len =
					(uint16_t)(last_seg->data_len -
					(ETHER_CRC_LEN - rx_packet_len));
				last_seg->next = NULL;
			} else
				rxm->data_len = (uint16_t)(rx_packet_len -
								ETHER_CRC_LEN);
		}

		first_seg->port = rxq->port_id;
		first_seg->ol_flags = 0;
		i40e_rxd_to_vlan_tci(first_seg, &rxd);
		pkt_flags = i40e_rxd_status_to_pkt_flags(qword1);
		pkt_flags |= i40e_rxd_error_to_pkt_flags(qword1);
		first_seg->packet_type =
			i40e_rxd_pkt_type_mapping((uint8_t)((qword1 &
			I40E_RXD_QW1_PTYPE_MASK) >> I40E_RXD_QW1_PTYPE_SHIFT));
		if (pkt_flags & PKT_RX_RSS_HASH)
			first_seg->hash.rss =
				rte_le_to_cpu_32(rxd.wb.qword0.hi_dword.rss);
		if (pkt_flags & PKT_RX_FDIR)
			pkt_flags |= i40e_rxd_build_fdir(&rxd, first_seg);

#ifdef RTE_LIBRTE_IEEE1588
		pkt_flags |= i40e_get_iee15888_flags(first_seg, qword1);
#endif
		first_seg->ol_flags |= pkt_flags;

		/* Prefetch data of first segment, if configured to do so. */
		rte_prefetch0(RTE_PTR_ADD(first_seg->buf_addr,
			first_seg->data_off));
		rx_pkts[nb_rx++] = first_seg;
		first_seg = NULL;
	}

	/* Record index of the next RX descriptor to probe. */
	rxq->rx_tail = rx_id;
	rxq->pkt_first_seg = first_seg;
	rxq->pkt_last_seg = last_seg;

	/**
	 * If the number of free RX descriptors is greater than the RX free
	 * threshold of the queue, advance the Receive Descriptor Tail (RDT)
	 * register. Update the RDT with the value of the last processed RX
	 * descriptor minus 1, to guarantee that the RDT register is never
	 * equal to the RDH register, which creates a "full" ring situtation
	 * from the hardware point of view.
	 */
	nb_hold = (uint16_t)(nb_hold + rxq->nb_rx_hold);
	if (nb_hold > rxq->rx_free_thresh) {
		rx_id = (uint16_t)(rx_id == 0 ?
			(rxq->nb_rx_desc - 1) : (rx_id - 1));
		I40E_PCI_REG_WRITE(rxq->qrx_tail, rx_id);
		nb_hold = 0;
	}
	rxq->nb_rx_hold = nb_hold;

	return nb_rx;
}

/* Check if the context descriptor is needed for TX offloading */
static inline uint16_t
i40e_calc_context_desc(uint64_t flags)
{
	static uint64_t mask = PKT_TX_OUTER_IP_CKSUM |
		PKT_TX_TCP_SEG |
		PKT_TX_QINQ_PKT |
		PKT_TX_TUNNEL_MASK;

#ifdef RTE_LIBRTE_IEEE1588
	mask |= PKT_TX_IEEE1588_TMST;
#endif

	return (flags & mask) ? 1 : 0;
}

/* set i40e TSO context descriptor */
static inline uint64_t
i40e_set_tso_ctx(struct rte_mbuf *mbuf, union i40e_tx_offload tx_offload)
{
	uint64_t ctx_desc = 0;
	uint32_t cd_cmd, hdr_len, cd_tso_len;

	if (!tx_offload.l4_len) {
		PMD_DRV_LOG(DEBUG, "L4 length set to 0");
		return ctx_desc;
	}

	/**
	 * in case of non tunneling packet, the outer_l2_len and
	 * outer_l3_len must be 0.
	 */
	hdr_len = tx_offload.outer_l2_len +
		tx_offload.outer_l3_len +
		tx_offload.l2_len +
		tx_offload.l3_len +
		tx_offload.l4_len;

	cd_cmd = I40E_TX_CTX_DESC_TSO;
	cd_tso_len = mbuf->pkt_len - hdr_len;
	ctx_desc |= ((uint64_t)cd_cmd << I40E_TXD_CTX_QW1_CMD_SHIFT) |
		((uint64_t)cd_tso_len <<
		 I40E_TXD_CTX_QW1_TSO_LEN_SHIFT) |
		((uint64_t)mbuf->tso_segsz <<
		 I40E_TXD_CTX_QW1_MSS_SHIFT);

	return ctx_desc;
}

uint16_t
i40e_xmit_pkts(void *tx_queue, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct i40e_tx_queue *txq;
	struct i40e_tx_entry *sw_ring;
	struct i40e_tx_entry *txe, *txn;
	volatile struct i40e_tx_desc *txd;
	volatile struct i40e_tx_desc *txr;
	struct rte_mbuf *tx_pkt;
	struct rte_mbuf *m_seg;
	uint32_t cd_tunneling_params;
	uint16_t tx_id;
	uint16_t nb_tx;
	uint32_t td_cmd;
	uint32_t td_offset;
	uint32_t tx_flags;
	uint32_t td_tag;
	uint64_t ol_flags;
	uint16_t nb_used;
	uint16_t nb_ctx;
	uint16_t tx_last;
	uint16_t slen;
	uint64_t buf_dma_addr;
	union i40e_tx_offload tx_offload = {0};

	txq = tx_queue;
	sw_ring = txq->sw_ring;
	txr = txq->tx_ring;
	tx_id = txq->tx_tail;
	txe = &sw_ring[tx_id];

	/* Check if the descriptor ring needs to be cleaned. */
	if (txq->nb_tx_free < txq->tx_free_thresh)
		i40e_xmit_cleanup(txq);

	for (nb_tx = 0; nb_tx < nb_pkts; nb_tx++) {
		td_cmd = 0;
		td_tag = 0;
		td_offset = 0;
		tx_flags = 0;

		tx_pkt = *tx_pkts++;
		RTE_MBUF_PREFETCH_TO_FREE(txe->mbuf);

		ol_flags = tx_pkt->ol_flags;
		tx_offload.l2_len = tx_pkt->l2_len;
		tx_offload.l3_len = tx_pkt->l3_len;
		tx_offload.outer_l2_len = tx_pkt->outer_l2_len;
		tx_offload.outer_l3_len = tx_pkt->outer_l3_len;
		tx_offload.l4_len = tx_pkt->l4_len;
		tx_offload.tso_segsz = tx_pkt->tso_segsz;

		/* Calculate the number of context descriptors needed. */
		nb_ctx = i40e_calc_context_desc(ol_flags);

		/**
		 * The number of descriptors that must be allocated for
		 * a packet equals to the number of the segments of that
		 * packet plus 1 context descriptor if needed.
		 */
		nb_used = (uint16_t)(tx_pkt->nb_segs + nb_ctx);
		tx_last = (uint16_t)(tx_id + nb_used - 1);

		/* Circular ring */
		if (tx_last >= txq->nb_tx_desc)
			tx_last = (uint16_t)(tx_last - txq->nb_tx_desc);

		if (nb_used > txq->nb_tx_free) {
			if (i40e_xmit_cleanup(txq) != 0) {
				if (nb_tx == 0)
					return 0;
				goto end_of_tx;
			}
			if (unlikely(nb_used > txq->tx_rs_thresh)) {
				while (nb_used > txq->nb_tx_free) {
					if (i40e_xmit_cleanup(txq) != 0) {
						if (nb_tx == 0)
							return 0;
						goto end_of_tx;
					}
				}
			}
		}

		/* Descriptor based VLAN insertion */
		if (ol_flags & (PKT_TX_VLAN_PKT | PKT_TX_QINQ_PKT)) {
			tx_flags |= tx_pkt->vlan_tci <<
				I40E_TX_FLAG_L2TAG1_SHIFT;
			tx_flags |= I40E_TX_FLAG_INSERT_VLAN;
			td_cmd |= I40E_TX_DESC_CMD_IL2TAG1;
			td_tag = (tx_flags & I40E_TX_FLAG_L2TAG1_MASK) >>
						I40E_TX_FLAG_L2TAG1_SHIFT;
		}

		/* Always enable CRC offload insertion */
		td_cmd |= I40E_TX_DESC_CMD_ICRC;

		/* Fill in tunneling parameters if necessary */
		cd_tunneling_params = 0;
		if (ol_flags & PKT_TX_TUNNEL_MASK)
			i40e_parse_tunneling_params(ol_flags, tx_offload,
						    &cd_tunneling_params);
		/* Enable checksum offloading */
		if (ol_flags & I40E_TX_CKSUM_OFFLOAD_MASK)
			i40e_txd_enable_checksum(ol_flags, &td_cmd,
						 &td_offset, tx_offload);

		if (nb_ctx) {
			/* Setup TX context descriptor if required */
			volatile struct i40e_tx_context_desc *ctx_txd =
				(volatile struct i40e_tx_context_desc *)\
							&txr[tx_id];
			uint16_t cd_l2tag2 = 0;
			uint64_t cd_type_cmd_tso_mss =
				I40E_TX_DESC_DTYPE_CONTEXT;

			txn = &sw_ring[txe->next_id];
			RTE_MBUF_PREFETCH_TO_FREE(txn->mbuf);
			if (txe->mbuf != NULL) {
				rte_pktmbuf_free_seg(txe->mbuf);
				txe->mbuf = NULL;
			}

			/* TSO enabled means no timestamp */
			if (ol_flags & PKT_TX_TCP_SEG)
				cd_type_cmd_tso_mss |=
					i40e_set_tso_ctx(tx_pkt, tx_offload);
			else {
#ifdef RTE_LIBRTE_IEEE1588
				if (ol_flags & PKT_TX_IEEE1588_TMST)
					cd_type_cmd_tso_mss |=
						((uint64_t)I40E_TX_CTX_DESC_TSYN <<
						 I40E_TXD_CTX_QW1_CMD_SHIFT);
#endif
			}

			ctx_txd->tunneling_params =
				rte_cpu_to_le_32(cd_tunneling_params);
			if (ol_flags & PKT_TX_QINQ_PKT) {
				cd_l2tag2 = tx_pkt->vlan_tci_outer;
				cd_type_cmd_tso_mss |=
					((uint64_t)I40E_TX_CTX_DESC_IL2TAG2 <<
						I40E_TXD_CTX_QW1_CMD_SHIFT);
			}
			ctx_txd->l2tag2 = rte_cpu_to_le_16(cd_l2tag2);
			ctx_txd->type_cmd_tso_mss =
				rte_cpu_to_le_64(cd_type_cmd_tso_mss);

			PMD_TX_LOG(DEBUG, "mbuf: %p, TCD[%u]:\n"
				"tunneling_params: %#x;\n"
				"l2tag2: %#hx;\n"
				"rsvd: %#hx;\n"
				"type_cmd_tso_mss: %#"PRIx64";\n",
				tx_pkt, tx_id,
				ctx_txd->tunneling_params,
				ctx_txd->l2tag2,
				ctx_txd->rsvd,
				ctx_txd->type_cmd_tso_mss);

			txe->last_id = tx_last;
			tx_id = txe->next_id;
			txe = txn;
		}

		m_seg = tx_pkt;
		do {
			txd = &txr[tx_id];
			txn = &sw_ring[txe->next_id];

			if (txe->mbuf)
				rte_pktmbuf_free_seg(txe->mbuf);
			txe->mbuf = m_seg;

			/* Setup TX Descriptor */
			slen = m_seg->data_len;
			buf_dma_addr = rte_mbuf_data_dma_addr(m_seg);

			PMD_TX_LOG(DEBUG, "mbuf: %p, TDD[%u]:\n"
				"buf_dma_addr: %#"PRIx64";\n"
				"td_cmd: %#x;\n"
				"td_offset: %#x;\n"
				"td_len: %u;\n"
				"td_tag: %#x;\n",
				tx_pkt, tx_id, buf_dma_addr,
				td_cmd, td_offset, slen, td_tag);

			txd->buffer_addr = rte_cpu_to_le_64(buf_dma_addr);
			txd->cmd_type_offset_bsz = i40e_build_ctob(td_cmd,
						td_offset, slen, td_tag);
			txe->last_id = tx_last;
			tx_id = txe->next_id;
			txe = txn;
			m_seg = m_seg->next;
		} while (m_seg != NULL);

		/* The last packet data descriptor needs End Of Packet (EOP) */
		td_cmd |= I40E_TX_DESC_CMD_EOP;
		txq->nb_tx_used = (uint16_t)(txq->nb_tx_used + nb_used);
		txq->nb_tx_free = (uint16_t)(txq->nb_tx_free - nb_used);

		if (txq->nb_tx_used >= txq->tx_rs_thresh) {
			PMD_TX_FREE_LOG(DEBUG,
					"Setting RS bit on TXD id="
					"%4u (port=%d queue=%d)",
					tx_last, txq->port_id, txq->queue_id);

			td_cmd |= I40E_TX_DESC_CMD_RS;

			/* Update txq RS bit counters */
			txq->nb_tx_used = 0;
		}

		txd->cmd_type_offset_bsz |=
			rte_cpu_to_le_64(((uint64_t)td_cmd) <<
					I40E_TXD_QW1_CMD_SHIFT);
	}

end_of_tx:
	rte_wmb();

	PMD_TX_LOG(DEBUG, "port_id=%u queue_id=%u tx_tail=%u nb_tx=%u",
		   (unsigned) txq->port_id, (unsigned) txq->queue_id,
		   (unsigned) tx_id, (unsigned) nb_tx);

	I40E_PCI_REG_WRITE_RELAXED(txq->qtx_tail, tx_id);
	txq->tx_tail = tx_id;

	return nb_tx;
}

static inline int __attribute__((always_inline))
i40e_tx_free_bufs(struct i40e_tx_queue *txq)
{
	struct i40e_tx_entry *txep;
	uint16_t i;

	if ((txq->tx_ring[txq->tx_next_dd].cmd_type_offset_bsz &
			rte_cpu_to_le_64(I40E_TXD_QW1_DTYPE_MASK)) !=
			rte_cpu_to_le_64(I40E_TX_DESC_DTYPE_DESC_DONE))
		return 0;

	txep = &(txq->sw_ring[txq->tx_next_dd - (txq->tx_rs_thresh - 1)]);

	for (i = 0; i < txq->tx_rs_thresh; i++)
		rte_prefetch0((txep + i)->mbuf);

	if (txq->txq_flags & (uint32_t)ETH_TXQ_FLAGS_NOREFCOUNT) {
		for (i = 0; i < txq->tx_rs_thresh; ++i, ++txep) {
			rte_mempool_put(txep->mbuf->pool, txep->mbuf);
			txep->mbuf = NULL;
		}
	} else {
		for (i = 0; i < txq->tx_rs_thresh; ++i, ++txep) {
			rte_pktmbuf_free_seg(txep->mbuf);
			txep->mbuf = NULL;
		}
	}

	txq->nb_tx_free = (uint16_t)(txq->nb_tx_free + txq->tx_rs_thresh);
	txq->tx_next_dd = (uint16_t)(txq->tx_next_dd + txq->tx_rs_thresh);
	if (txq->tx_next_dd >= txq->nb_tx_desc)
		txq->tx_next_dd = (uint16_t)(txq->tx_rs_thresh - 1);

	return txq->tx_rs_thresh;
}

/* Populate 4 descriptors with data from 4 mbufs */
static inline void
tx4(volatile struct i40e_tx_desc *txdp, struct rte_mbuf **pkts)
{
	uint64_t dma_addr;
	uint32_t i;

	for (i = 0; i < 4; i++, txdp++, pkts++) {
		dma_addr = rte_mbuf_data_dma_addr(*pkts);
		txdp->buffer_addr = rte_cpu_to_le_64(dma_addr);
		txdp->cmd_type_offset_bsz =
			i40e_build_ctob((uint32_t)I40E_TD_CMD, 0,
					(*pkts)->data_len, 0);
	}
}

/* Populate 1 descriptor with data from 1 mbuf */
static inline void
tx1(volatile struct i40e_tx_desc *txdp, struct rte_mbuf **pkts)
{
	uint64_t dma_addr;

	dma_addr = rte_mbuf_data_dma_addr(*pkts);
	txdp->buffer_addr = rte_cpu_to_le_64(dma_addr);
	txdp->cmd_type_offset_bsz =
		i40e_build_ctob((uint32_t)I40E_TD_CMD, 0,
				(*pkts)->data_len, 0);
}

/* Fill hardware descriptor ring with mbuf data */
static inline void
i40e_tx_fill_hw_ring(struct i40e_tx_queue *txq,
		     struct rte_mbuf **pkts,
		     uint16_t nb_pkts)
{
	volatile struct i40e_tx_desc *txdp = &(txq->tx_ring[txq->tx_tail]);
	struct i40e_tx_entry *txep = &(txq->sw_ring[txq->tx_tail]);
	const int N_PER_LOOP = 4;
	const int N_PER_LOOP_MASK = N_PER_LOOP - 1;
	int mainpart, leftover;
	int i, j;

	mainpart = (nb_pkts & ((uint32_t) ~N_PER_LOOP_MASK));
	leftover = (nb_pkts & ((uint32_t)  N_PER_LOOP_MASK));
	for (i = 0; i < mainpart; i += N_PER_LOOP) {
		for (j = 0; j < N_PER_LOOP; ++j) {
			(txep + i + j)->mbuf = *(pkts + i + j);
		}
		tx4(txdp + i, pkts + i);
	}
	if (unlikely(leftover > 0)) {
		for (i = 0; i < leftover; ++i) {
			(txep + mainpart + i)->mbuf = *(pkts + mainpart + i);
			tx1(txdp + mainpart + i, pkts + mainpart + i);
		}
	}
}

static inline uint16_t
tx_xmit_pkts(struct i40e_tx_queue *txq,
	     struct rte_mbuf **tx_pkts,
	     uint16_t nb_pkts)
{
	volatile struct i40e_tx_desc *txr = txq->tx_ring;
	uint16_t n = 0;

	/**
	 * Begin scanning the H/W ring for done descriptors when the number
	 * of available descriptors drops below tx_free_thresh. For each done
	 * descriptor, free the associated buffer.
	 */
	if (txq->nb_tx_free < txq->tx_free_thresh)
		i40e_tx_free_bufs(txq);

	/* Use available descriptor only */
	nb_pkts = (uint16_t)RTE_MIN(txq->nb_tx_free, nb_pkts);
	if (unlikely(!nb_pkts))
		return 0;

	txq->nb_tx_free = (uint16_t)(txq->nb_tx_free - nb_pkts);
	if ((txq->tx_tail + nb_pkts) > txq->nb_tx_desc) {
		n = (uint16_t)(txq->nb_tx_desc - txq->tx_tail);
		i40e_tx_fill_hw_ring(txq, tx_pkts, n);
		txr[txq->tx_next_rs].cmd_type_offset_bsz |=
			rte_cpu_to_le_64(((uint64_t)I40E_TX_DESC_CMD_RS) <<
						I40E_TXD_QW1_CMD_SHIFT);
		txq->tx_next_rs = (uint16_t)(txq->tx_rs_thresh - 1);
		txq->tx_tail = 0;
	}

	/* Fill hardware descriptor ring with mbuf data */
	i40e_tx_fill_hw_ring(txq, tx_pkts + n, (uint16_t)(nb_pkts - n));
	txq->tx_tail = (uint16_t)(txq->tx_tail + (nb_pkts - n));

	/* Determin if RS bit needs to be set */
	if (txq->tx_tail > txq->tx_next_rs) {
		txr[txq->tx_next_rs].cmd_type_offset_bsz |=
			rte_cpu_to_le_64(((uint64_t)I40E_TX_DESC_CMD_RS) <<
						I40E_TXD_QW1_CMD_SHIFT);
		txq->tx_next_rs =
			(uint16_t)(txq->tx_next_rs + txq->tx_rs_thresh);
		if (txq->tx_next_rs >= txq->nb_tx_desc)
			txq->tx_next_rs = (uint16_t)(txq->tx_rs_thresh - 1);
	}

	if (txq->tx_tail >= txq->nb_tx_desc)
		txq->tx_tail = 0;

	/* Update the tx tail register */
	rte_wmb();
	I40E_PCI_REG_WRITE_RELAXED(txq->qtx_tail, txq->tx_tail);

	return nb_pkts;
}

static uint16_t
i40e_xmit_pkts_simple(void *tx_queue,
		      struct rte_mbuf **tx_pkts,
		      uint16_t nb_pkts)
{
	uint16_t nb_tx = 0;

	if (likely(nb_pkts <= I40E_TX_MAX_BURST))
		return tx_xmit_pkts((struct i40e_tx_queue *)tx_queue,
						tx_pkts, nb_pkts);

	while (nb_pkts) {
		uint16_t ret, num = (uint16_t)RTE_MIN(nb_pkts,
						I40E_TX_MAX_BURST);

		ret = tx_xmit_pkts((struct i40e_tx_queue *)tx_queue,
						&tx_pkts[nb_tx], num);
		nb_tx = (uint16_t)(nb_tx + ret);
		nb_pkts = (uint16_t)(nb_pkts - ret);
		if (ret < num)
			break;
	}

	return nb_tx;
}

/*********************************************************************
 *
 *  TX prep functions
 *
 **********************************************************************/
uint16_t
i40e_prep_pkts(__rte_unused void *tx_queue, struct rte_mbuf **tx_pkts,
		uint16_t nb_pkts)
{
	int i, ret;
	uint64_t ol_flags;
	struct rte_mbuf *m;

	for (i = 0; i < nb_pkts; i++) {
		m = tx_pkts[i];
		ol_flags = m->ol_flags;

		/**
		 * m->nb_segs is uint8_t, so nb_segs is always less than
		 * I40E_TX_MAX_SEG.
		 * We check only a condition for nb_segs > I40E_TX_MAX_MTU_SEG.
		 */
		if (!(ol_flags & PKT_TX_TCP_SEG)) {
			if (m->nb_segs > I40E_TX_MAX_MTU_SEG) {
				rte_errno = -EINVAL;
				return i;
			}
		} else if ((m->tso_segsz < I40E_MIN_TSO_MSS) ||
				(m->tso_segsz > I40E_MAX_TSO_MSS)) {
			/* MSS outside the range (256B - 9674B) are considered
			 * malicious
			 */
			rte_errno = -EINVAL;
			return i;
		}

		if (ol_flags & I40E_TX_OFFLOAD_NOTSUP_MASK) {
			rte_errno = -ENOTSUP;
			return i;
		}

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
		ret = rte_validate_tx_offload(m);
		if (ret != 0) {
			rte_errno = ret;
			return i;
		}
#endif
		ret = rte_net_intel_cksum_prepare(m);
		if (ret != 0) {
			rte_errno = ret;
			return i;
		}
	}
	return i;
}

/*
 * Find the VSI the queue belongs to. 'queue_idx' is the queue index
 * application used, which assume having sequential ones. But from driver's
 * perspective, it's different. For example, q0 belongs to FDIR VSI, q1-q64
 * to MAIN VSI, , q65-96 to SRIOV VSIs, q97-128 to VMDQ VSIs. For application
 * running on host, q1-64 and q97-128 can be used, total 96 queues. They can
 * use queue_idx from 0 to 95 to access queues, while real queue would be
 * different. This function will do a queue mapping to find VSI the queue
 * belongs to.
 */
static struct i40e_vsi*
i40e_pf_get_vsi_by_qindex(struct i40e_pf *pf, uint16_t queue_idx)
{
	/* the queue in MAIN VSI range */
	if (queue_idx < pf->main_vsi->nb_qps)
		return pf->main_vsi;

	queue_idx -= pf->main_vsi->nb_qps;

	/* queue_idx is greater than VMDQ VSIs range */
	if (queue_idx > pf->nb_cfg_vmdq_vsi * pf->vmdq_nb_qps - 1) {
		PMD_INIT_LOG(ERR, "queue_idx out of range. VMDQ configured?");
		return NULL;
	}

	return pf->vmdq[queue_idx / pf->vmdq_nb_qps].vsi;
}

static uint16_t
i40e_get_queue_offset_by_qindex(struct i40e_pf *pf, uint16_t queue_idx)
{
	/* the queue in MAIN VSI range */
	if (queue_idx < pf->main_vsi->nb_qps)
		return queue_idx;

	/* It's VMDQ queues */
	queue_idx -= pf->main_vsi->nb_qps;

	if (pf->nb_cfg_vmdq_vsi)
		return queue_idx % pf->vmdq_nb_qps;
	else {
		PMD_INIT_LOG(ERR, "Fail to get queue offset");
		return (uint16_t)(-1);
	}
}

int
i40e_dev_rx_queue_start(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct i40e_rx_queue *rxq;
	int err = -1;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (rx_queue_id < dev->data->nb_rx_queues) {
		rxq = dev->data->rx_queues[rx_queue_id];

		err = i40e_alloc_rx_queue_mbufs(rxq);
		if (err) {
			PMD_DRV_LOG(ERR, "Failed to allocate RX queue mbuf");
			return err;
		}

		rte_wmb();

		/* Init the RX tail regieter. */
		I40E_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);

		err = i40e_switch_rx_queue(hw, rxq->reg_idx, TRUE);

		if (err) {
			PMD_DRV_LOG(ERR, "Failed to switch RX queue %u on",
				    rx_queue_id);

			i40e_rx_queue_release_mbufs(rxq);
			i40e_reset_rx_queue(rxq);
		} else
			dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;
	}

	return err;
}

int
i40e_dev_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct i40e_rx_queue *rxq;
	int err;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (rx_queue_id < dev->data->nb_rx_queues) {
		rxq = dev->data->rx_queues[rx_queue_id];

		/*
		* rx_queue_id is queue id aplication refers to, while
		* rxq->reg_idx is the real queue index.
		*/
		err = i40e_switch_rx_queue(hw, rxq->reg_idx, FALSE);

		if (err) {
			PMD_DRV_LOG(ERR, "Failed to switch RX queue %u off",
				    rx_queue_id);
			return err;
		}
		i40e_rx_queue_release_mbufs(rxq);
		i40e_reset_rx_queue(rxq);
		dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}

	return 0;
}

int
i40e_dev_tx_queue_start(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	int err = -1;
	struct i40e_tx_queue *txq;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (tx_queue_id < dev->data->nb_tx_queues) {
		txq = dev->data->tx_queues[tx_queue_id];

		/*
		* tx_queue_id is queue id aplication refers to, while
		* rxq->reg_idx is the real queue index.
		*/
		err = i40e_switch_tx_queue(hw, txq->reg_idx, TRUE);
		if (err)
			PMD_DRV_LOG(ERR, "Failed to switch TX queue %u on",
				    tx_queue_id);
		else
			dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;
	}

	return err;
}

int
i40e_dev_tx_queue_stop(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	struct i40e_tx_queue *txq;
	int err;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	if (tx_queue_id < dev->data->nb_tx_queues) {
		txq = dev->data->tx_queues[tx_queue_id];

		/*
		* tx_queue_id is queue id aplication refers to, while
		* txq->reg_idx is the real queue index.
		*/
		err = i40e_switch_tx_queue(hw, txq->reg_idx, FALSE);

		if (err) {
			PMD_DRV_LOG(ERR, "Failed to switch TX queue %u of",
				    tx_queue_id);
			return err;
		}

		i40e_tx_queue_release_mbufs(txq);
		i40e_reset_tx_queue(txq);
		dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}

	return 0;
}

const uint32_t *
i40e_dev_supported_ptypes_get(struct rte_eth_dev *dev)
{
	static const uint32_t ptypes[] = {
		/* refers to i40e_rxd_pkt_type_mapping() */
		RTE_PTYPE_L2_ETHER,
		RTE_PTYPE_L2_ETHER_TIMESYNC,
		RTE_PTYPE_L2_ETHER_LLDP,
		RTE_PTYPE_L2_ETHER_ARP,
		RTE_PTYPE_L3_IPV4_EXT_UNKNOWN,
		RTE_PTYPE_L3_IPV6_EXT_UNKNOWN,
		RTE_PTYPE_L4_FRAG,
		RTE_PTYPE_L4_ICMP,
		RTE_PTYPE_L4_NONFRAG,
		RTE_PTYPE_L4_SCTP,
		RTE_PTYPE_L4_TCP,
		RTE_PTYPE_L4_UDP,
		RTE_PTYPE_TUNNEL_GRENAT,
		RTE_PTYPE_TUNNEL_IP,
		RTE_PTYPE_INNER_L2_ETHER,
		RTE_PTYPE_INNER_L2_ETHER_VLAN,
		RTE_PTYPE_INNER_L3_IPV4_EXT_UNKNOWN,
		RTE_PTYPE_INNER_L3_IPV6_EXT_UNKNOWN,
		RTE_PTYPE_INNER_L4_FRAG,
		RTE_PTYPE_INNER_L4_ICMP,
		RTE_PTYPE_INNER_L4_NONFRAG,
		RTE_PTYPE_INNER_L4_SCTP,
		RTE_PTYPE_INNER_L4_TCP,
		RTE_PTYPE_INNER_L4_UDP,
		RTE_PTYPE_UNKNOWN
	};

	if (dev->rx_pkt_burst == i40e_recv_pkts ||
#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	    dev->rx_pkt_burst == i40e_recv_pkts_bulk_alloc ||
#endif
	    dev->rx_pkt_burst == i40e_recv_scattered_pkts ||
	    dev->rx_pkt_burst == i40e_recv_scattered_pkts_vec ||
	    dev->rx_pkt_burst == i40e_recv_pkts_vec)
		return ptypes;
	return NULL;
}

int
i40e_dev_rx_queue_setup(struct rte_eth_dev *dev,
			uint16_t queue_idx,
			uint16_t nb_desc,
			unsigned int socket_id,
			const struct rte_eth_rxconf *rx_conf,
			struct rte_mempool *mp)
{
	struct i40e_vsi *vsi;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_pf *pf = I40E_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	struct i40e_adapter *ad =
		I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);
	struct i40e_rx_queue *rxq;
	const struct rte_memzone *rz;
	uint32_t ring_size;
	uint16_t len, i;
	uint16_t base, bsf, tc_mapping;
	int use_def_burst_func = 1;

	if (hw->mac.type == I40E_MAC_VF || hw->mac.type == I40E_MAC_X722_VF) {
		struct i40e_vf *vf =
			I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
		vsi = &vf->vsi;
	} else
		vsi = i40e_pf_get_vsi_by_qindex(pf, queue_idx);

	if (vsi == NULL) {
		PMD_DRV_LOG(ERR, "VSI not available or queue "
			    "index exceeds the maximum");
		return I40E_ERR_PARAM;
	}
	if (nb_desc % I40E_ALIGN_RING_DESC != 0 ||
			(nb_desc > I40E_MAX_RING_DESC) ||
			(nb_desc < I40E_MIN_RING_DESC)) {
		PMD_DRV_LOG(ERR, "Number (%u) of receive descriptors is "
			    "invalid", nb_desc);
		return I40E_ERR_PARAM;
	}

	/* Free memory if needed */
	if (dev->data->rx_queues[queue_idx]) {
		i40e_dev_rx_queue_release(dev->data->rx_queues[queue_idx]);
		dev->data->rx_queues[queue_idx] = NULL;
	}

	/* Allocate the rx queue data structure */
	rxq = rte_zmalloc_socket("i40e rx queue",
				 sizeof(struct i40e_rx_queue),
				 RTE_CACHE_LINE_SIZE,
				 socket_id);
	if (!rxq) {
		PMD_DRV_LOG(ERR, "Failed to allocate memory for "
			    "rx queue data structure");
		return -ENOMEM;
	}
	rxq->mp = mp;
	rxq->nb_rx_desc = nb_desc;
	rxq->rx_free_thresh = rx_conf->rx_free_thresh;
	rxq->queue_id = queue_idx;
	if (hw->mac.type == I40E_MAC_VF || hw->mac.type == I40E_MAC_X722_VF)
		rxq->reg_idx = queue_idx;
	else /* PF device */
		rxq->reg_idx = vsi->base_queue +
			i40e_get_queue_offset_by_qindex(pf, queue_idx);

	rxq->port_id = dev->data->port_id;
	rxq->crc_len = (uint8_t) ((dev->data->dev_conf.rxmode.hw_strip_crc) ?
							0 : ETHER_CRC_LEN);
	rxq->drop_en = rx_conf->rx_drop_en;
	rxq->vsi = vsi;
	rxq->rx_deferred_start = rx_conf->rx_deferred_start;

	/* Allocate the maximun number of RX ring hardware descriptor. */
	len = I40E_MAX_RING_DESC;

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	/**
	 * Allocating a little more memory because vectorized/bulk_alloc Rx
	 * functions doesn't check boundaries each time.
	 */
	len += RTE_PMD_I40E_RX_MAX_BURST;
#endif

	ring_size = RTE_ALIGN(len * sizeof(union i40e_rx_desc),
			      I40E_DMA_MEM_ALIGN);

	rz = rte_eth_dma_zone_reserve(dev, "rx_ring", queue_idx,
			      ring_size, I40E_RING_BASE_ALIGN, socket_id);
	if (!rz) {
		i40e_dev_rx_queue_release(rxq);
		PMD_DRV_LOG(ERR, "Failed to reserve DMA memory for RX");
		return -ENOMEM;
	}

	/* Zero all the descriptors in the ring. */
	memset(rz->addr, 0, ring_size);

	rxq->rx_ring_phys_addr = rte_mem_phy2mch(rz->memseg_id, rz->phys_addr);
	rxq->rx_ring = (union i40e_rx_desc *)rz->addr;

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	len = (uint16_t)(nb_desc + RTE_PMD_I40E_RX_MAX_BURST);
#else
	len = nb_desc;
#endif

	/* Allocate the software ring. */
	rxq->sw_ring =
		rte_zmalloc_socket("i40e rx sw ring",
				   sizeof(struct i40e_rx_entry) * len,
				   RTE_CACHE_LINE_SIZE,
				   socket_id);
	if (!rxq->sw_ring) {
		i40e_dev_rx_queue_release(rxq);
		PMD_DRV_LOG(ERR, "Failed to allocate memory for SW ring");
		return -ENOMEM;
	}

	i40e_reset_rx_queue(rxq);
	rxq->q_set = TRUE;
	dev->data->rx_queues[queue_idx] = rxq;

	use_def_burst_func = check_rx_burst_bulk_alloc_preconditions(rxq);

	if (!use_def_burst_func) {
#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are "
			     "satisfied. Rx Burst Bulk Alloc function will be "
			     "used on port=%d, queue=%d.",
			     rxq->port_id, rxq->queue_id);
#endif /* RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC */
	} else {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are "
			     "not satisfied, Scattered Rx is requested, "
			     "or RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC is "
			     "not enabled on port=%d, queue=%d.",
			     rxq->port_id, rxq->queue_id);
		ad->rx_bulk_alloc_allowed = false;
	}

	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
		if (!(vsi->enabled_tc & (1 << i)))
			continue;
		tc_mapping = rte_le_to_cpu_16(vsi->info.tc_mapping[i]);
		base = (tc_mapping & I40E_AQ_VSI_TC_QUE_OFFSET_MASK) >>
			I40E_AQ_VSI_TC_QUE_OFFSET_SHIFT;
		bsf = (tc_mapping & I40E_AQ_VSI_TC_QUE_NUMBER_MASK) >>
			I40E_AQ_VSI_TC_QUE_NUMBER_SHIFT;

		if (queue_idx >= base && queue_idx < (base + BIT(bsf)))
			rxq->dcb_tc = i;
	}

	return 0;
}

void
i40e_dev_rx_queue_release(void *rxq)
{
	struct i40e_rx_queue *q = (struct i40e_rx_queue *)rxq;

	if (!q) {
		PMD_DRV_LOG(DEBUG, "Pointer to rxq is NULL");
		return;
	}

	i40e_rx_queue_release_mbufs(q);
	rte_free(q->sw_ring);
	rte_free(q);
}

uint32_t
i40e_dev_rx_queue_count(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
#define I40E_RXQ_SCAN_INTERVAL 4
	volatile union i40e_rx_desc *rxdp;
	struct i40e_rx_queue *rxq;
	uint16_t desc = 0;

	if (unlikely(rx_queue_id >= dev->data->nb_rx_queues)) {
		PMD_DRV_LOG(ERR, "Invalid RX queue id %u", rx_queue_id);
		return 0;
	}

	rxq = dev->data->rx_queues[rx_queue_id];
	rxdp = &(rxq->rx_ring[rxq->rx_tail]);
	while ((desc < rxq->nb_rx_desc) &&
		((rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len) &
		I40E_RXD_QW1_STATUS_MASK) >> I40E_RXD_QW1_STATUS_SHIFT) &
				(1 << I40E_RX_DESC_STATUS_DD_SHIFT)) {
		/**
		 * Check the DD bit of a rx descriptor of each 4 in a group,
		 * to avoid checking too frequently and downgrading performance
		 * too much.
		 */
		desc += I40E_RXQ_SCAN_INTERVAL;
		rxdp += I40E_RXQ_SCAN_INTERVAL;
		if (rxq->rx_tail + desc >= rxq->nb_rx_desc)
			rxdp = &(rxq->rx_ring[rxq->rx_tail +
					desc - rxq->nb_rx_desc]);
	}

	return desc;
}

int
i40e_dev_rx_descriptor_done(void *rx_queue, uint16_t offset)
{
	volatile union i40e_rx_desc *rxdp;
	struct i40e_rx_queue *rxq = rx_queue;
	uint16_t desc;
	int ret;

	if (unlikely(offset >= rxq->nb_rx_desc)) {
		PMD_DRV_LOG(ERR, "Invalid RX queue id %u", offset);
		return 0;
	}

	desc = rxq->rx_tail + offset;
	if (desc >= rxq->nb_rx_desc)
		desc -= rxq->nb_rx_desc;

	rxdp = &(rxq->rx_ring[desc]);

	ret = !!(((rte_le_to_cpu_64(rxdp->wb.qword1.status_error_len) &
		I40E_RXD_QW1_STATUS_MASK) >> I40E_RXD_QW1_STATUS_SHIFT) &
				(1 << I40E_RX_DESC_STATUS_DD_SHIFT));

	return ret;
}

int
i40e_dev_tx_queue_setup(struct rte_eth_dev *dev,
			uint16_t queue_idx,
			uint16_t nb_desc,
			unsigned int socket_id,
			const struct rte_eth_txconf *tx_conf)
{
	struct i40e_vsi *vsi;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_pf *pf = I40E_DEV_PRIVATE_TO_PF(dev->data->dev_private);
	struct i40e_tx_queue *txq;
	const struct rte_memzone *tz;
	uint32_t ring_size;
	uint16_t tx_rs_thresh, tx_free_thresh;
	uint16_t i, base, bsf, tc_mapping;

	if (hw->mac.type == I40E_MAC_VF || hw->mac.type == I40E_MAC_X722_VF) {
		struct i40e_vf *vf =
			I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
		vsi = &vf->vsi;
	} else
		vsi = i40e_pf_get_vsi_by_qindex(pf, queue_idx);

	if (vsi == NULL) {
		PMD_DRV_LOG(ERR, "VSI is NULL, or queue index (%u) "
			    "exceeds the maximum", queue_idx);
		return I40E_ERR_PARAM;
	}

	if (nb_desc % I40E_ALIGN_RING_DESC != 0 ||
			(nb_desc > I40E_MAX_RING_DESC) ||
			(nb_desc < I40E_MIN_RING_DESC)) {
		PMD_DRV_LOG(ERR, "Number (%u) of transmit descriptors is "
			    "invalid", nb_desc);
		return I40E_ERR_PARAM;
	}

	/**
	 * The following two parameters control the setting of the RS bit on
	 * transmit descriptors. TX descriptors will have their RS bit set
	 * after txq->tx_rs_thresh descriptors have been used. The TX
	 * descriptor ring will be cleaned after txq->tx_free_thresh
	 * descriptors are used or if the number of descriptors required to
	 * transmit a packet is greater than the number of free TX descriptors.
	 *
	 * The following constraints must be satisfied:
	 *  - tx_rs_thresh must be greater than 0.
	 *  - tx_rs_thresh must be less than the size of the ring minus 2.
	 *  - tx_rs_thresh must be less than or equal to tx_free_thresh.
	 *  - tx_rs_thresh must be a divisor of the ring size.
	 *  - tx_free_thresh must be greater than 0.
	 *  - tx_free_thresh must be less than the size of the ring minus 3.
	 *
	 * One descriptor in the TX ring is used as a sentinel to avoid a H/W
	 * race condition, hence the maximum threshold constraints. When set
	 * to zero use default values.
	 */
	tx_rs_thresh = (uint16_t)((tx_conf->tx_rs_thresh) ?
		tx_conf->tx_rs_thresh : DEFAULT_TX_RS_THRESH);
	tx_free_thresh = (uint16_t)((tx_conf->tx_free_thresh) ?
		tx_conf->tx_free_thresh : DEFAULT_TX_FREE_THRESH);
	if (tx_rs_thresh >= (nb_desc - 2)) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh must be less than the "
			     "number of TX descriptors minus 2. "
			     "(tx_rs_thresh=%u port=%d queue=%d)",
			     (unsigned int)tx_rs_thresh,
			     (int)dev->data->port_id,
			     (int)queue_idx);
		return I40E_ERR_PARAM;
	}
	if (tx_free_thresh >= (nb_desc - 3)) {
		PMD_INIT_LOG(ERR, "tx_free_thresh must be less than the "
			     "number of TX descriptors minus 3. "
			     "(tx_free_thresh=%u port=%d queue=%d)",
			     (unsigned int)tx_free_thresh,
			     (int)dev->data->port_id,
			     (int)queue_idx);
		return I40E_ERR_PARAM;
	}
	if (tx_rs_thresh > tx_free_thresh) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh must be less than or "
			     "equal to tx_free_thresh. (tx_free_thresh=%u"
			     " tx_rs_thresh=%u port=%d queue=%d)",
			     (unsigned int)tx_free_thresh,
			     (unsigned int)tx_rs_thresh,
			     (int)dev->data->port_id,
			     (int)queue_idx);
		return I40E_ERR_PARAM;
	}
	if ((nb_desc % tx_rs_thresh) != 0) {
		PMD_INIT_LOG(ERR, "tx_rs_thresh must be a divisor of the "
			     "number of TX descriptors. (tx_rs_thresh=%u"
			     " port=%d queue=%d)",
			     (unsigned int)tx_rs_thresh,
			     (int)dev->data->port_id,
			     (int)queue_idx);
		return I40E_ERR_PARAM;
	}
	if ((tx_rs_thresh > 1) && (tx_conf->tx_thresh.wthresh != 0)) {
		PMD_INIT_LOG(ERR, "TX WTHRESH must be set to 0 if "
			     "tx_rs_thresh is greater than 1. "
			     "(tx_rs_thresh=%u port=%d queue=%d)",
			     (unsigned int)tx_rs_thresh,
			     (int)dev->data->port_id,
			     (int)queue_idx);
		return I40E_ERR_PARAM;
	}

	/* Free memory if needed. */
	if (dev->data->tx_queues[queue_idx]) {
		i40e_dev_tx_queue_release(dev->data->tx_queues[queue_idx]);
		dev->data->tx_queues[queue_idx] = NULL;
	}

	/* Allocate the TX queue data structure. */
	txq = rte_zmalloc_socket("i40e tx queue",
				  sizeof(struct i40e_tx_queue),
				  RTE_CACHE_LINE_SIZE,
				  socket_id);
	if (!txq) {
		PMD_DRV_LOG(ERR, "Failed to allocate memory for "
			    "tx queue structure");
		return -ENOMEM;
	}

	/* Allocate TX hardware ring descriptors. */
	ring_size = sizeof(struct i40e_tx_desc) * I40E_MAX_RING_DESC;
	ring_size = RTE_ALIGN(ring_size, I40E_DMA_MEM_ALIGN);
	tz = rte_eth_dma_zone_reserve(dev, "tx_ring", queue_idx,
			      ring_size, I40E_RING_BASE_ALIGN, socket_id);
	if (!tz) {
		i40e_dev_tx_queue_release(txq);
		PMD_DRV_LOG(ERR, "Failed to reserve DMA memory for TX");
		return -ENOMEM;
	}

	txq->nb_tx_desc = nb_desc;
	txq->tx_rs_thresh = tx_rs_thresh;
	txq->tx_free_thresh = tx_free_thresh;
	txq->pthresh = tx_conf->tx_thresh.pthresh;
	txq->hthresh = tx_conf->tx_thresh.hthresh;
	txq->wthresh = tx_conf->tx_thresh.wthresh;
	txq->queue_id = queue_idx;
	if (hw->mac.type == I40E_MAC_VF || hw->mac.type == I40E_MAC_X722_VF)
		txq->reg_idx = queue_idx;
	else /* PF device */
		txq->reg_idx = vsi->base_queue +
			i40e_get_queue_offset_by_qindex(pf, queue_idx);

	txq->port_id = dev->data->port_id;
	txq->txq_flags = tx_conf->txq_flags;
	txq->vsi = vsi;
	txq->tx_deferred_start = tx_conf->tx_deferred_start;

	txq->tx_ring_phys_addr = rte_mem_phy2mch(tz->memseg_id, tz->phys_addr);
	txq->tx_ring = (struct i40e_tx_desc *)tz->addr;

	/* Allocate software ring */
	txq->sw_ring =
		rte_zmalloc_socket("i40e tx sw ring",
				   sizeof(struct i40e_tx_entry) * nb_desc,
				   RTE_CACHE_LINE_SIZE,
				   socket_id);
	if (!txq->sw_ring) {
		i40e_dev_tx_queue_release(txq);
		PMD_DRV_LOG(ERR, "Failed to allocate memory for SW TX ring");
		return -ENOMEM;
	}

	i40e_reset_tx_queue(txq);
	txq->q_set = TRUE;
	dev->data->tx_queues[queue_idx] = txq;

	/* Use a simple TX queue without offloads or multi segs if possible */
	i40e_set_tx_function_flag(dev, txq);

	for (i = 0; i < I40E_MAX_TRAFFIC_CLASS; i++) {
		if (!(vsi->enabled_tc & (1 << i)))
			continue;
		tc_mapping = rte_le_to_cpu_16(vsi->info.tc_mapping[i]);
		base = (tc_mapping & I40E_AQ_VSI_TC_QUE_OFFSET_MASK) >>
			I40E_AQ_VSI_TC_QUE_OFFSET_SHIFT;
		bsf = (tc_mapping & I40E_AQ_VSI_TC_QUE_NUMBER_MASK) >>
			I40E_AQ_VSI_TC_QUE_NUMBER_SHIFT;

		if (queue_idx >= base && queue_idx < (base + BIT(bsf)))
			txq->dcb_tc = i;
	}

	return 0;
}

void
i40e_dev_tx_queue_release(void *txq)
{
	struct i40e_tx_queue *q = (struct i40e_tx_queue *)txq;

	if (!q) {
		PMD_DRV_LOG(DEBUG, "Pointer to TX queue is NULL");
		return;
	}

	i40e_tx_queue_release_mbufs(q);
	rte_free(q->sw_ring);
	rte_free(q);
}

const struct rte_memzone *
i40e_memzone_reserve(const char *name, uint32_t len, int socket_id)
{
	const struct rte_memzone *mz;

	mz = rte_memzone_lookup(name);
	if (mz)
		return mz;

	if (rte_xen_dom0_supported())
		mz = rte_memzone_reserve_bounded(name, len,
				socket_id, 0, I40E_RING_BASE_ALIGN, RTE_PGSIZE_2M);
	else
		mz = rte_memzone_reserve_aligned(name, len,
				socket_id, 0, I40E_RING_BASE_ALIGN);
	return mz;
}

void
i40e_rx_queue_release_mbufs(struct i40e_rx_queue *rxq)
{
	uint16_t i;

	/* SSE Vector driver has a different way of releasing mbufs. */
	if (rxq->rx_using_sse) {
		i40e_rx_queue_release_mbufs_vec(rxq);
		return;
	}

	if (!rxq->sw_ring) {
		PMD_DRV_LOG(DEBUG, "Pointer to sw_ring is NULL");
		return;
	}

	for (i = 0; i < rxq->nb_rx_desc; i++) {
		if (rxq->sw_ring[i].mbuf) {
			rte_pktmbuf_free_seg(rxq->sw_ring[i].mbuf);
			rxq->sw_ring[i].mbuf = NULL;
		}
	}
#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	if (rxq->rx_nb_avail == 0)
		return;
	for (i = 0; i < rxq->rx_nb_avail; i++) {
		struct rte_mbuf *mbuf;

		mbuf = rxq->rx_stage[rxq->rx_next_avail + i];
		rte_pktmbuf_free_seg(mbuf);
	}
	rxq->rx_nb_avail = 0;
#endif /* RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC */
}

void
i40e_reset_rx_queue(struct i40e_rx_queue *rxq)
{
	unsigned i;
	uint16_t len;

	if (!rxq) {
		PMD_DRV_LOG(DEBUG, "Pointer to rxq is NULL");
		return;
	}

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	if (check_rx_burst_bulk_alloc_preconditions(rxq) == 0)
		len = (uint16_t)(rxq->nb_rx_desc + RTE_PMD_I40E_RX_MAX_BURST);
	else
#endif /* RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC */
		len = rxq->nb_rx_desc;

	for (i = 0; i < len * sizeof(union i40e_rx_desc); i++)
		((volatile char *)rxq->rx_ring)[i] = 0;

#ifdef RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC
	memset(&rxq->fake_mbuf, 0x0, sizeof(rxq->fake_mbuf));
	for (i = 0; i < RTE_PMD_I40E_RX_MAX_BURST; ++i)
		rxq->sw_ring[rxq->nb_rx_desc + i].mbuf = &rxq->fake_mbuf;

	rxq->rx_nb_avail = 0;
	rxq->rx_next_avail = 0;
	rxq->rx_free_trigger = (uint16_t)(rxq->rx_free_thresh - 1);
#endif /* RTE_LIBRTE_I40E_RX_ALLOW_BULK_ALLOC */
	rxq->rx_tail = 0;
	rxq->nb_rx_hold = 0;
	rxq->pkt_first_seg = NULL;
	rxq->pkt_last_seg = NULL;

	rxq->rxrearm_start = 0;
	rxq->rxrearm_nb = 0;
}

void
i40e_tx_queue_release_mbufs(struct i40e_tx_queue *txq)
{
	uint16_t i;

	if (!txq || !txq->sw_ring) {
		PMD_DRV_LOG(DEBUG, "Pointer to rxq or sw_ring is NULL");
		return;
	}

	for (i = 0; i < txq->nb_tx_desc; i++) {
		if (txq->sw_ring[i].mbuf) {
			rte_pktmbuf_free_seg(txq->sw_ring[i].mbuf);
			txq->sw_ring[i].mbuf = NULL;
		}
	}
}

void
i40e_reset_tx_queue(struct i40e_tx_queue *txq)
{
	struct i40e_tx_entry *txe;
	uint16_t i, prev, size;

	if (!txq) {
		PMD_DRV_LOG(DEBUG, "Pointer to txq is NULL");
		return;
	}

	txe = txq->sw_ring;
	size = sizeof(struct i40e_tx_desc) * txq->nb_tx_desc;
	for (i = 0; i < size; i++)
		((volatile char *)txq->tx_ring)[i] = 0;

	prev = (uint16_t)(txq->nb_tx_desc - 1);
	for (i = 0; i < txq->nb_tx_desc; i++) {
		volatile struct i40e_tx_desc *txd = &txq->tx_ring[i];

		txd->cmd_type_offset_bsz =
			rte_cpu_to_le_64(I40E_TX_DESC_DTYPE_DESC_DONE);
		txe[i].mbuf =  NULL;
		txe[i].last_id = i;
		txe[prev].next_id = i;
		prev = i;
	}

	txq->tx_next_dd = (uint16_t)(txq->tx_rs_thresh - 1);
	txq->tx_next_rs = (uint16_t)(txq->tx_rs_thresh - 1);

	txq->tx_tail = 0;
	txq->nb_tx_used = 0;

	txq->last_desc_cleaned = (uint16_t)(txq->nb_tx_desc - 1);
	txq->nb_tx_free = (uint16_t)(txq->nb_tx_desc - 1);
}

/* Init the TX queue in hardware */
int
i40e_tx_queue_init(struct i40e_tx_queue *txq)
{
	enum i40e_status_code err = I40E_SUCCESS;
	struct i40e_vsi *vsi = txq->vsi;
	struct i40e_hw *hw = I40E_VSI_TO_HW(vsi);
	uint16_t pf_q = txq->reg_idx;
	struct i40e_hmc_obj_txq tx_ctx;
	uint32_t qtx_ctl;

	/* clear the context structure first */
	memset(&tx_ctx, 0, sizeof(tx_ctx));
	tx_ctx.new_context = 1;
	tx_ctx.base = txq->tx_ring_phys_addr / I40E_QUEUE_BASE_ADDR_UNIT;
	tx_ctx.qlen = txq->nb_tx_desc;

#ifdef RTE_LIBRTE_IEEE1588
	tx_ctx.timesync_ena = 1;
#endif
	tx_ctx.rdylist = rte_le_to_cpu_16(vsi->info.qs_handle[txq->dcb_tc]);
	if (vsi->type == I40E_VSI_FDIR)
		tx_ctx.fd_ena = TRUE;

	err = i40e_clear_lan_tx_queue_context(hw, pf_q);
	if (err != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Failure of clean lan tx queue context");
		return err;
	}

	err = i40e_set_lan_tx_queue_context(hw, pf_q, &tx_ctx);
	if (err != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Failure of set lan tx queue context");
		return err;
	}

	/* Now associate this queue with this PCI function */
	qtx_ctl = I40E_QTX_CTL_PF_QUEUE;
	qtx_ctl |= ((hw->pf_id << I40E_QTX_CTL_PF_INDX_SHIFT) &
					I40E_QTX_CTL_PF_INDX_MASK);
	I40E_WRITE_REG(hw, I40E_QTX_CTL(pf_q), qtx_ctl);
	I40E_WRITE_FLUSH(hw);

	txq->qtx_tail = hw->hw_addr + I40E_QTX_TAIL(pf_q);

	return err;
}

int
i40e_alloc_rx_queue_mbufs(struct i40e_rx_queue *rxq)
{
	struct i40e_rx_entry *rxe = rxq->sw_ring;
	uint64_t dma_addr;
	uint16_t i;

	for (i = 0; i < rxq->nb_rx_desc; i++) {
		volatile union i40e_rx_desc *rxd;
		struct rte_mbuf *mbuf = rte_mbuf_raw_alloc(rxq->mp);

		if (unlikely(!mbuf)) {
			PMD_DRV_LOG(ERR, "Failed to allocate mbuf for RX");
			return -ENOMEM;
		}

		rte_mbuf_refcnt_set(mbuf, 1);
		mbuf->next = NULL;
		mbuf->data_off = RTE_PKTMBUF_HEADROOM;
		mbuf->nb_segs = 1;
		mbuf->port = rxq->port_id;

		dma_addr =
			rte_cpu_to_le_64(rte_mbuf_data_dma_addr_default(mbuf));

		rxd = &rxq->rx_ring[i];
		rxd->read.pkt_addr = dma_addr;
		rxd->read.hdr_addr = 0;
#ifndef RTE_LIBRTE_I40E_16BYTE_RX_DESC
		rxd->read.rsvd1 = 0;
		rxd->read.rsvd2 = 0;
#endif /* RTE_LIBRTE_I40E_16BYTE_RX_DESC */

		rxe[i].mbuf = mbuf;
	}

	return 0;
}

/*
 * Calculate the buffer length, and check the jumbo frame
 * and maximum packet length.
 */
static int
i40e_rx_queue_config(struct i40e_rx_queue *rxq)
{
	struct i40e_pf *pf = I40E_VSI_TO_PF(rxq->vsi);
	struct i40e_hw *hw = I40E_VSI_TO_HW(rxq->vsi);
	struct rte_eth_dev_data *data = pf->dev_data;
	uint16_t buf_size, len;

	buf_size = (uint16_t)(rte_pktmbuf_data_room_size(rxq->mp) -
		RTE_PKTMBUF_HEADROOM);

	switch (pf->flags & (I40E_FLAG_HEADER_SPLIT_DISABLED |
			I40E_FLAG_HEADER_SPLIT_ENABLED)) {
	case I40E_FLAG_HEADER_SPLIT_ENABLED: /* Not supported */
		rxq->rx_hdr_len = RTE_ALIGN(I40E_RXBUF_SZ_1024,
				(1 << I40E_RXQ_CTX_HBUFF_SHIFT));
		rxq->rx_buf_len = RTE_ALIGN(I40E_RXBUF_SZ_2048,
				(1 << I40E_RXQ_CTX_DBUFF_SHIFT));
		rxq->hs_mode = i40e_header_split_enabled;
		break;
	case I40E_FLAG_HEADER_SPLIT_DISABLED:
	default:
		rxq->rx_hdr_len = 0;
		rxq->rx_buf_len = RTE_ALIGN(buf_size,
			(1 << I40E_RXQ_CTX_DBUFF_SHIFT));
		rxq->hs_mode = i40e_header_split_none;
		break;
	}

	len = hw->func_caps.rx_buf_chain_len * rxq->rx_buf_len;
	rxq->max_pkt_len = RTE_MIN(len, data->dev_conf.rxmode.max_rx_pkt_len);
	if (data->dev_conf.rxmode.jumbo_frame == 1) {
		if (rxq->max_pkt_len <= ETHER_MAX_LEN ||
			rxq->max_pkt_len > I40E_FRAME_SIZE_MAX) {
			PMD_DRV_LOG(ERR, "maximum packet length must "
				    "be larger than %u and smaller than %u,"
				    "as jumbo frame is enabled",
				    (uint32_t)ETHER_MAX_LEN,
				    (uint32_t)I40E_FRAME_SIZE_MAX);
			return I40E_ERR_CONFIG;
		}
	} else {
		if (rxq->max_pkt_len < ETHER_MIN_LEN ||
			rxq->max_pkt_len > ETHER_MAX_LEN) {
			PMD_DRV_LOG(ERR, "maximum packet length must be "
				    "larger than %u and smaller than %u, "
				    "as jumbo frame is disabled",
				    (uint32_t)ETHER_MIN_LEN,
				    (uint32_t)ETHER_MAX_LEN);
			return I40E_ERR_CONFIG;
		}
	}

	return 0;
}

/* Init the RX queue in hardware */
int
i40e_rx_queue_init(struct i40e_rx_queue *rxq)
{
	int err = I40E_SUCCESS;
	struct i40e_hw *hw = I40E_VSI_TO_HW(rxq->vsi);
	struct rte_eth_dev_data *dev_data = I40E_VSI_TO_DEV_DATA(rxq->vsi);
	uint16_t pf_q = rxq->reg_idx;
	uint16_t buf_size;
	struct i40e_hmc_obj_rxq rx_ctx;

	err = i40e_rx_queue_config(rxq);
	if (err < 0) {
		PMD_DRV_LOG(ERR, "Failed to config RX queue");
		return err;
	}

	/* Clear the context structure first */
	memset(&rx_ctx, 0, sizeof(struct i40e_hmc_obj_rxq));
	rx_ctx.dbuff = rxq->rx_buf_len >> I40E_RXQ_CTX_DBUFF_SHIFT;
	rx_ctx.hbuff = rxq->rx_hdr_len >> I40E_RXQ_CTX_HBUFF_SHIFT;

	rx_ctx.base = rxq->rx_ring_phys_addr / I40E_QUEUE_BASE_ADDR_UNIT;
	rx_ctx.qlen = rxq->nb_rx_desc;
#ifndef RTE_LIBRTE_I40E_16BYTE_RX_DESC
	rx_ctx.dsize = 1;
#endif
	rx_ctx.dtype = rxq->hs_mode;
	if (rxq->hs_mode)
		rx_ctx.hsplit_0 = I40E_HEADER_SPLIT_ALL;
	else
		rx_ctx.hsplit_0 = I40E_HEADER_SPLIT_NONE;
	rx_ctx.rxmax = rxq->max_pkt_len + I40E_VLAN_TAG_SIZE;
	rx_ctx.tphrdesc_ena = 1;
	rx_ctx.tphwdesc_ena = 1;
	rx_ctx.tphdata_ena = 1;
	rx_ctx.tphhead_ena = 1;
	rx_ctx.lrxqthresh = 2;
	rx_ctx.crcstrip = (rxq->crc_len == 0) ? 1 : 0;
	rx_ctx.l2tsel = 1;
	/* showiv indicates if inner VLAN is stripped inside of tunnel
	 * packet. When set it to 1, vlan information is stripped from
	 * the inner header, but the hardware does not put it in the
	 * descriptor. So set it zero by default.
	 */
	rx_ctx.showiv = 0;
	rx_ctx.prefena = 1;

	err = i40e_clear_lan_rx_queue_context(hw, pf_q);
	if (err != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Failed to clear LAN RX queue context");
		return err;
	}
	err = i40e_set_lan_rx_queue_context(hw, pf_q, &rx_ctx);
	if (err != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Failed to set LAN RX queue context");
		return err;
	}

	rxq->qrx_tail = hw->hw_addr + I40E_QRX_TAIL(pf_q);

	buf_size = (uint16_t)(rte_pktmbuf_data_room_size(rxq->mp) -
		RTE_PKTMBUF_HEADROOM);

	/* Check if scattered RX needs to be used. */
	if ((rxq->max_pkt_len + 2 * I40E_VLAN_TAG_SIZE) > buf_size) {
		dev_data->scattered_rx = 1;
	}

	/* Init the RX tail regieter. */
	I40E_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);

	return 0;
}

void
i40e_dev_clear_queues(struct rte_eth_dev *dev)
{
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		if (!dev->data->tx_queues[i])
			continue;
		i40e_tx_queue_release_mbufs(dev->data->tx_queues[i]);
		i40e_reset_tx_queue(dev->data->tx_queues[i]);
	}

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		if (!dev->data->rx_queues[i])
			continue;
		i40e_rx_queue_release_mbufs(dev->data->rx_queues[i]);
		i40e_reset_rx_queue(dev->data->rx_queues[i]);
	}
}

void
i40e_dev_free_queues(struct rte_eth_dev *dev)
{
	uint16_t i;

	PMD_INIT_FUNC_TRACE();

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		if (!dev->data->rx_queues[i])
			continue;
		i40e_dev_rx_queue_release(dev->data->rx_queues[i]);
		dev->data->rx_queues[i] = NULL;
	}
	dev->data->nb_rx_queues = 0;

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		if (!dev->data->tx_queues[i])
			continue;
		i40e_dev_tx_queue_release(dev->data->tx_queues[i]);
		dev->data->tx_queues[i] = NULL;
	}
	dev->data->nb_tx_queues = 0;
}

#define I40E_FDIR_NUM_TX_DESC  I40E_MIN_RING_DESC
#define I40E_FDIR_NUM_RX_DESC  I40E_MIN_RING_DESC

enum i40e_status_code
i40e_fdir_setup_tx_resources(struct i40e_pf *pf)
{
	struct i40e_tx_queue *txq;
	const struct rte_memzone *tz = NULL;
	uint32_t ring_size;
	struct rte_eth_dev *dev;

	if (!pf) {
		PMD_DRV_LOG(ERR, "PF is not available");
		return I40E_ERR_BAD_PTR;
	}

	dev = pf->adapter->eth_dev;

	/* Allocate the TX queue data structure. */
	txq = rte_zmalloc_socket("i40e fdir tx queue",
				  sizeof(struct i40e_tx_queue),
				  RTE_CACHE_LINE_SIZE,
				  SOCKET_ID_ANY);
	if (!txq) {
		PMD_DRV_LOG(ERR, "Failed to allocate memory for "
					"tx queue structure.");
		return I40E_ERR_NO_MEMORY;
	}

	/* Allocate TX hardware ring descriptors. */
	ring_size = sizeof(struct i40e_tx_desc) * I40E_FDIR_NUM_TX_DESC;
	ring_size = RTE_ALIGN(ring_size, I40E_DMA_MEM_ALIGN);

	tz = rte_eth_dma_zone_reserve(dev, "fdir_tx_ring",
				      I40E_FDIR_QUEUE_ID, ring_size,
				      I40E_RING_BASE_ALIGN, SOCKET_ID_ANY);
	if (!tz) {
		i40e_dev_tx_queue_release(txq);
		PMD_DRV_LOG(ERR, "Failed to reserve DMA memory for TX.");
		return I40E_ERR_NO_MEMORY;
	}

	txq->nb_tx_desc = I40E_FDIR_NUM_TX_DESC;
	txq->queue_id = I40E_FDIR_QUEUE_ID;
	txq->reg_idx = pf->fdir.fdir_vsi->base_queue;
	txq->vsi = pf->fdir.fdir_vsi;

	txq->tx_ring_phys_addr = rte_mem_phy2mch(tz->memseg_id, tz->phys_addr);
	txq->tx_ring = (struct i40e_tx_desc *)tz->addr;
	/*
	 * don't need to allocate software ring and reset for the fdir
	 * program queue just set the queue has been configured.
	 */
	txq->q_set = TRUE;
	pf->fdir.txq = txq;

	return I40E_SUCCESS;
}

enum i40e_status_code
i40e_fdir_setup_rx_resources(struct i40e_pf *pf)
{
	struct i40e_rx_queue *rxq;
	const struct rte_memzone *rz = NULL;
	uint32_t ring_size;
	struct rte_eth_dev *dev;

	if (!pf) {
		PMD_DRV_LOG(ERR, "PF is not available");
		return I40E_ERR_BAD_PTR;
	}

	dev = pf->adapter->eth_dev;

	/* Allocate the RX queue data structure. */
	rxq = rte_zmalloc_socket("i40e fdir rx queue",
				  sizeof(struct i40e_rx_queue),
				  RTE_CACHE_LINE_SIZE,
				  SOCKET_ID_ANY);
	if (!rxq) {
		PMD_DRV_LOG(ERR, "Failed to allocate memory for "
					"rx queue structure.");
		return I40E_ERR_NO_MEMORY;
	}

	/* Allocate RX hardware ring descriptors. */
	ring_size = sizeof(union i40e_rx_desc) * I40E_FDIR_NUM_RX_DESC;
	ring_size = RTE_ALIGN(ring_size, I40E_DMA_MEM_ALIGN);

	rz = rte_eth_dma_zone_reserve(dev, "fdir_rx_ring",
				      I40E_FDIR_QUEUE_ID, ring_size,
				      I40E_RING_BASE_ALIGN, SOCKET_ID_ANY);
	if (!rz) {
		i40e_dev_rx_queue_release(rxq);
		PMD_DRV_LOG(ERR, "Failed to reserve DMA memory for RX.");
		return I40E_ERR_NO_MEMORY;
	}

	rxq->nb_rx_desc = I40E_FDIR_NUM_RX_DESC;
	rxq->queue_id = I40E_FDIR_QUEUE_ID;
	rxq->reg_idx = pf->fdir.fdir_vsi->base_queue;
	rxq->vsi = pf->fdir.fdir_vsi;

	rxq->rx_ring_phys_addr = rte_mem_phy2mch(rz->memseg_id, rz->phys_addr);
	rxq->rx_ring = (union i40e_rx_desc *)rz->addr;

	/*
	 * Don't need to allocate software ring and reset for the fdir
	 * rx queue, just set the queue has been configured.
	 */
	rxq->q_set = TRUE;
	pf->fdir.rxq = rxq;

	return I40E_SUCCESS;
}

void
i40e_rxq_info_get(struct rte_eth_dev *dev, uint16_t queue_id,
	struct rte_eth_rxq_info *qinfo)
{
	struct i40e_rx_queue *rxq;

	rxq = dev->data->rx_queues[queue_id];

	qinfo->mp = rxq->mp;
	qinfo->scattered_rx = dev->data->scattered_rx;
	qinfo->nb_desc = rxq->nb_rx_desc;

	qinfo->conf.rx_free_thresh = rxq->rx_free_thresh;
	qinfo->conf.rx_drop_en = rxq->drop_en;
	qinfo->conf.rx_deferred_start = rxq->rx_deferred_start;
}

void
i40e_txq_info_get(struct rte_eth_dev *dev, uint16_t queue_id,
	struct rte_eth_txq_info *qinfo)
{
	struct i40e_tx_queue *txq;

	txq = dev->data->tx_queues[queue_id];

	qinfo->nb_desc = txq->nb_tx_desc;

	qinfo->conf.tx_thresh.pthresh = txq->pthresh;
	qinfo->conf.tx_thresh.hthresh = txq->hthresh;
	qinfo->conf.tx_thresh.wthresh = txq->wthresh;

	qinfo->conf.tx_free_thresh = txq->tx_free_thresh;
	qinfo->conf.tx_rs_thresh = txq->tx_rs_thresh;
	qinfo->conf.txq_flags = txq->txq_flags;
	qinfo->conf.tx_deferred_start = txq->tx_deferred_start;
}

void __attribute__((cold))
i40e_set_rx_function(struct rte_eth_dev *dev)
{
	struct i40e_adapter *ad =
		I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);
	uint16_t rx_using_sse, i;
	/* In order to allow Vector Rx there are a few configuration
	 * conditions to be met and Rx Bulk Allocation should be allowed.
	 */
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		if (i40e_rx_vec_dev_conf_condition_check(dev) ||
		    !ad->rx_bulk_alloc_allowed) {
			PMD_INIT_LOG(DEBUG, "Port[%d] doesn't meet"
				     " Vector Rx preconditions",
				     dev->data->port_id);

			ad->rx_vec_allowed = false;
		}
		if (ad->rx_vec_allowed) {
			for (i = 0; i < dev->data->nb_rx_queues; i++) {
				struct i40e_rx_queue *rxq =
					dev->data->rx_queues[i];

				if (rxq && i40e_rxq_vec_setup(rxq)) {
					ad->rx_vec_allowed = false;
					break;
				}
			}
		}
	}

	if (dev->data->scattered_rx) {
		/* Set the non-LRO scattered callback: there are Vector and
		 * single allocation versions.
		 */
		if (ad->rx_vec_allowed) {
			PMD_INIT_LOG(DEBUG, "Using Vector Scattered Rx "
					    "callback (port=%d).",
				     dev->data->port_id);

			dev->rx_pkt_burst = i40e_recv_scattered_pkts_vec;
		} else {
			PMD_INIT_LOG(DEBUG, "Using a Scattered with bulk "
					   "allocation callback (port=%d).",
				     dev->data->port_id);
			dev->rx_pkt_burst = i40e_recv_scattered_pkts;
		}
	/* If parameters allow we are going to choose between the following
	 * callbacks:
	 *    - Vector
	 *    - Bulk Allocation
	 *    - Single buffer allocation (the simplest one)
	 */
	} else if (ad->rx_vec_allowed) {
		PMD_INIT_LOG(DEBUG, "Vector rx enabled, please make sure RX "
				    "burst size no less than %d (port=%d).",
			     RTE_I40E_DESCS_PER_LOOP,
			     dev->data->port_id);

		dev->rx_pkt_burst = i40e_recv_pkts_vec;
	} else if (ad->rx_bulk_alloc_allowed) {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are "
				    "satisfied. Rx Burst Bulk Alloc function "
				    "will be used on port=%d.",
			     dev->data->port_id);

		dev->rx_pkt_burst = i40e_recv_pkts_bulk_alloc;
	} else {
		PMD_INIT_LOG(DEBUG, "Rx Burst Bulk Alloc Preconditions are not "
				    "satisfied, or Scattered Rx is requested "
				    "(port=%d).",
			     dev->data->port_id);

		dev->rx_pkt_burst = i40e_recv_pkts;
	}

	/* Propagate information about RX function choice through all queues. */
	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		rx_using_sse =
			(dev->rx_pkt_burst == i40e_recv_scattered_pkts_vec ||
			 dev->rx_pkt_burst == i40e_recv_pkts_vec);

		for (i = 0; i < dev->data->nb_rx_queues; i++) {
			struct i40e_rx_queue *rxq = dev->data->rx_queues[i];

			if (rxq)
				rxq->rx_using_sse = rx_using_sse;
		}
	}
}

void __attribute__((cold))
i40e_set_tx_function_flag(struct rte_eth_dev *dev, struct i40e_tx_queue *txq)
{
	struct i40e_adapter *ad =
		I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);

	/* Use a simple Tx queue (no offloads, no multi segs) if possible */
	if (((txq->txq_flags & I40E_SIMPLE_FLAGS) == I40E_SIMPLE_FLAGS)
			&& (txq->tx_rs_thresh >= RTE_PMD_I40E_TX_MAX_BURST)) {
		if (txq->tx_rs_thresh <= RTE_I40E_TX_MAX_FREE_BUF_SZ) {
			PMD_INIT_LOG(DEBUG, "Vector tx"
				     " can be enabled on this txq.");

		} else {
			ad->tx_vec_allowed = false;
		}
	} else {
		ad->tx_simple_allowed = false;
	}
}

void __attribute__((cold))
i40e_set_tx_function(struct rte_eth_dev *dev)
{
	struct i40e_adapter *ad =
		I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);
	int i;

	if (rte_eal_process_type() == RTE_PROC_PRIMARY) {
		if (ad->tx_vec_allowed) {
			for (i = 0; i < dev->data->nb_tx_queues; i++) {
				struct i40e_tx_queue *txq =
					dev->data->tx_queues[i];

				if (txq && i40e_txq_vec_setup(txq)) {
					ad->tx_vec_allowed = false;
					break;
				}
			}
		}
	}

	if (ad->tx_simple_allowed) {
		if (ad->tx_vec_allowed) {
			PMD_INIT_LOG(DEBUG, "Vector tx finally be used.");
			dev->tx_pkt_burst = i40e_xmit_pkts_vec;
		} else {
			PMD_INIT_LOG(DEBUG, "Simple tx finally be used.");
			dev->tx_pkt_burst = i40e_xmit_pkts_simple;
		}
		dev->tx_pkt_prepare = NULL;
	} else {
		PMD_INIT_LOG(DEBUG, "Xmit tx finally be used.");
		dev->tx_pkt_burst = i40e_xmit_pkts;
		dev->tx_pkt_prepare = i40e_prep_pkts;
	}
}

/* Stubs needed for linkage when CONFIG_RTE_I40E_INC_VECTOR is set to 'n' */
int __attribute__((weak))
i40e_rx_vec_dev_conf_condition_check(struct rte_eth_dev __rte_unused *dev)
{
	return -1;
}

uint16_t __attribute__((weak))
i40e_recv_pkts_vec(
	void __rte_unused *rx_queue,
	struct rte_mbuf __rte_unused **rx_pkts,
	uint16_t __rte_unused nb_pkts)
{
	return 0;
}

uint16_t __attribute__((weak))
i40e_recv_scattered_pkts_vec(
	void __rte_unused *rx_queue,
	struct rte_mbuf __rte_unused **rx_pkts,
	uint16_t __rte_unused nb_pkts)
{
	return 0;
}

int __attribute__((weak))
i40e_rxq_vec_setup(struct i40e_rx_queue __rte_unused *rxq)
{
	return -1;
}

int __attribute__((weak))
i40e_txq_vec_setup(struct i40e_tx_queue __rte_unused *txq)
{
	return -1;
}

void __attribute__((weak))
i40e_rx_queue_release_mbufs_vec(struct i40e_rx_queue __rte_unused*rxq)
{
	return;
}

uint16_t __attribute__((weak))
i40e_xmit_pkts_vec(void __rte_unused *tx_queue,
		   struct rte_mbuf __rte_unused **tx_pkts,
		   uint16_t __rte_unused nb_pkts)
{
	return 0;
}
