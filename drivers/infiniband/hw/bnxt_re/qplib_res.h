/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2016 - 2017, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: QPLib resource manager (header)
 */

#ifndef __BNXT_QPLIB_RES_H__
#define __BNXT_QPLIB_RES_H__

#include "bnxt_ulp.h"

extern const struct bnxt_qplib_gid bnxt_qplib_gid_zero;

#define CHIP_NUM_57508		0x1750
#define CHIP_NUM_57504		0x1751
#define CHIP_NUM_57502		0x1752
#define CHIP_NUM_58818          0xd818
#define CHIP_NUM_57608          0x1760

#define BNXT_RE_MAX_QPC_COUNT		(64 * 1024)
#define BNXT_RE_MAX_MRW_COUNT		(64 * 1024)
#define BNXT_RE_MAX_SRQC_COUNT		(64 * 1024)
#define BNXT_RE_MAX_CQ_COUNT		(64 * 1024)
#define BNXT_RE_MAX_MRW_COUNT_64K	(64 * 1024)
#define BNXT_RE_MAX_MRW_COUNT_256K	(256 * 1024)

#define BNXT_QPLIB_DBR_VALID		(0x1UL << 26)
#define BNXT_QPLIB_DBR_EPOCH_SHIFT	24
#define BNXT_QPLIB_DBR_TOGGLE_SHIFT	25

struct bnxt_qplib_drv_modes {
	u8	wqe_mode;
	bool db_push;
	bool dbr_pacing;
	u32 toggle_bits;
};

enum bnxt_re_toggle_modes {
	BNXT_QPLIB_CQ_TOGGLE_BIT = 0x1,
	BNXT_QPLIB_SRQ_TOGGLE_BIT = 0x2,
};

struct bnxt_qplib_chip_ctx {
	u16	chip_num;
	u8	chip_rev;
	u8	chip_metal;
	u16	hw_stats_size;
	u16	hwrm_cmd_max_timeout;
	struct bnxt_qplib_drv_modes modes;
	u64	hwrm_intf_ver;
	u32     dbr_stat_db_fifo;
};

struct bnxt_qplib_db_pacing_data {
	u32 do_pacing;
	u32 pacing_th;
	u32 alarm_th;
	u32 fifo_max_depth;
	u32 fifo_room_mask;
	u32 fifo_room_shift;
	u32 grc_reg_offset;
	u32 dev_err_state;
};

#define BNXT_QPLIB_DBR_PF_DB_OFFSET     0x10000
#define BNXT_QPLIB_DBR_VF_DB_OFFSET     0x4000

#define PTR_CNT_PER_PG		(PAGE_SIZE / sizeof(void *))
#define PTR_MAX_IDX_PER_PG	(PTR_CNT_PER_PG - 1)
#define PTR_PG(x)		(((x) & ~PTR_MAX_IDX_PER_PG) / PTR_CNT_PER_PG)
#define PTR_IDX(x)		((x) & PTR_MAX_IDX_PER_PG)

#define HWQ_CMP(idx, hwq)	((idx) & ((hwq)->max_elements - 1))

#define HWQ_FREE_SLOTS(hwq)	(hwq->max_elements - \
				((HWQ_CMP(hwq->prod, hwq)\
				- HWQ_CMP(hwq->cons, hwq))\
				& (hwq->max_elements - 1)))
enum bnxt_qplib_hwq_type {
	HWQ_TYPE_CTX,
	HWQ_TYPE_QUEUE,
	HWQ_TYPE_L2_CMPL,
	HWQ_TYPE_MR
};

#define MAX_PBL_LVL_0_PGS		1
#define MAX_PBL_LVL_1_PGS		512
#define MAX_PBL_LVL_1_PGS_SHIFT		9
#define MAX_PBL_LVL_1_PGS_FOR_LVL_2	256
#define MAX_PBL_LVL_2_PGS		(256 * 512)
#define MAX_PDL_LVL_SHIFT               9

enum bnxt_qplib_pbl_lvl {
	PBL_LVL_0,
	PBL_LVL_1,
	PBL_LVL_2,
	PBL_LVL_MAX
};

#define ROCE_PG_SIZE_4K		(4 * 1024)
#define ROCE_PG_SIZE_8K		(8 * 1024)
#define ROCE_PG_SIZE_64K	(64 * 1024)
#define ROCE_PG_SIZE_2M		(2 * 1024 * 1024)
#define ROCE_PG_SIZE_8M		(8 * 1024 * 1024)
#define ROCE_PG_SIZE_1G		(1024 * 1024 * 1024)

enum bnxt_qplib_hwrm_pg_size {
	BNXT_QPLIB_HWRM_PG_SIZE_4K	= 0,
	BNXT_QPLIB_HWRM_PG_SIZE_8K	= 1,
	BNXT_QPLIB_HWRM_PG_SIZE_64K	= 2,
	BNXT_QPLIB_HWRM_PG_SIZE_2M	= 3,
	BNXT_QPLIB_HWRM_PG_SIZE_8M	= 4,
	BNXT_QPLIB_HWRM_PG_SIZE_1G	= 5,
};

struct bnxt_qplib_reg_desc {
	u8		bar_id;
	resource_size_t	bar_base;
	unsigned long	offset;
	void __iomem	*bar_reg;
	size_t		len;
};

struct bnxt_qplib_pbl {
	u32				pg_count;
	u32				pg_size;
	void				**pg_arr;
	dma_addr_t			*pg_map_arr;
};

struct bnxt_qplib_sg_info {
	struct ib_umem			*umem;
	u32				npages;
	u32				pgshft;
	u32				pgsize;
	bool				nopte;
};

struct bnxt_qplib_hwq_attr {
	struct bnxt_qplib_res		*res;
	struct bnxt_qplib_sg_info	*sginfo;
	enum bnxt_qplib_hwq_type	type;
	u32				depth;
	u32				stride;
	u32				aux_stride;
	u32				aux_depth;
};

struct bnxt_qplib_hwq {
	struct pci_dev			*pdev;
	/* lock to protect qplib_hwq */
	spinlock_t			lock;
	struct bnxt_qplib_pbl		pbl[PBL_LVL_MAX + 1];
	enum bnxt_qplib_pbl_lvl		level;		/* 0, 1, or 2 */
	/* ptr for easy access to the PBL entries */
	void				**pbl_ptr;
	/* ptr for easy access to the dma_addr */
	dma_addr_t			*pbl_dma_ptr;
	u32				max_elements;
	u32				depth;
	u16				element_size;	/* Size of each entry */
	u16				qe_ppg;	/* queue entry per page */

	u32				prod;		/* raw */
	u32				cons;		/* raw */
	u8				cp_bit;
	u8				is_user;
	u64				*pad_pg;
	u32				pad_stride;
	u32				pad_pgofft;
};

struct bnxt_qplib_db_info {
	void __iomem		*db;
	void __iomem		*priv_db;
	struct bnxt_qplib_hwq	*hwq;
	u32			xid;
	u32			max_slot;
	u32                     flags;
	u8			toggle;
};

enum bnxt_qplib_db_info_flags_mask {
	BNXT_QPLIB_FLAG_EPOCH_CONS_SHIFT        = 0x0UL,
	BNXT_QPLIB_FLAG_EPOCH_PROD_SHIFT        = 0x1UL,
	BNXT_QPLIB_FLAG_EPOCH_CONS_MASK         = 0x1UL,
	BNXT_QPLIB_FLAG_EPOCH_PROD_MASK         = 0x2UL,
};

enum bnxt_qplib_db_epoch_flag_shift {
	BNXT_QPLIB_DB_EPOCH_CONS_SHIFT  = BNXT_QPLIB_DBR_EPOCH_SHIFT,
	BNXT_QPLIB_DB_EPOCH_PROD_SHIFT  = (BNXT_QPLIB_DBR_EPOCH_SHIFT - 1),
};

/* Tables */
struct bnxt_qplib_pd_tbl {
	unsigned long			*tbl;
	u32				max;
};

struct bnxt_qplib_sgid_tbl {
	struct bnxt_qplib_gid_info	*tbl;
	u16				*hw_id;
	u16				max;
	u16				active;
	void				*ctx;
	u8				*vlan;
};

enum {
	BNXT_QPLIB_DPI_TYPE_KERNEL      = 0,
	BNXT_QPLIB_DPI_TYPE_UC          = 1,
	BNXT_QPLIB_DPI_TYPE_WC          = 2
};

struct bnxt_qplib_dpi {
	u32				dpi;
	u32				bit;
	void __iomem			*dbr;
	u64				umdbr;
	u8				type;
};

struct bnxt_qplib_dpi_tbl {
	void				**app_tbl;
	unsigned long			*tbl;
	u16				max;
	struct bnxt_qplib_reg_desc	ucreg; /* Hold entire DB bar. */
	struct bnxt_qplib_reg_desc	wcreg;
	void __iomem			*priv_db;
};

struct bnxt_qplib_stats {
	dma_addr_t			dma_map;
	void				*dma;
	u32				size;
	u32				fw_id;
};

struct bnxt_qplib_vf_res {
	u32 max_qp_per_vf;
	u32 max_mrw_per_vf;
	u32 max_srq_per_vf;
	u32 max_cq_per_vf;
	u32 max_gid_per_vf;
};

#define BNXT_QPLIB_MAX_QP_CTX_ENTRY_SIZE	448
#define BNXT_QPLIB_MAX_SRQ_CTX_ENTRY_SIZE	64
#define BNXT_QPLIB_MAX_CQ_CTX_ENTRY_SIZE	64
#define BNXT_QPLIB_MAX_MRW_CTX_ENTRY_SIZE	128

#define MAX_TQM_ALLOC_REQ               48
#define MAX_TQM_ALLOC_BLK_SIZE          8
struct bnxt_qplib_tqm_ctx {
	struct bnxt_qplib_hwq           pde;
	u8                              pde_level; /* Original level */
	struct bnxt_qplib_hwq           qtbl[MAX_TQM_ALLOC_REQ];
	u8                              qcount[MAX_TQM_ALLOC_REQ];
};

struct bnxt_qplib_ctx {
	u32				qpc_count;
	struct bnxt_qplib_hwq		qpc_tbl;
	u32				mrw_count;
	struct bnxt_qplib_hwq		mrw_tbl;
	u32				srqc_count;
	struct bnxt_qplib_hwq		srqc_tbl;
	u32				cq_count;
	struct bnxt_qplib_hwq		cq_tbl;
	struct bnxt_qplib_hwq		tim_tbl;
	struct bnxt_qplib_tqm_ctx	tqm_ctx;
	struct bnxt_qplib_stats		stats;
	struct bnxt_qplib_vf_res	vf_res;
};

struct bnxt_qplib_res {
	struct pci_dev			*pdev;
	struct bnxt_qplib_chip_ctx	*cctx;
	struct bnxt_qplib_dev_attr      *dattr;
	struct net_device		*netdev;
	struct bnxt_en_dev		*en_dev;
	struct bnxt_qplib_rcfw		*rcfw;
	struct bnxt_qplib_pd_tbl	pd_tbl;
	/* To protect the pd table bit map */
	struct mutex			pd_tbl_lock;
	struct bnxt_qplib_sgid_tbl	sgid_tbl;
	struct bnxt_qplib_dpi_tbl	dpi_tbl;
	/* To protect the dpi table bit map */
	struct mutex                    dpi_tbl_lock;
	bool				prio;
	bool                            is_vf;
	struct bnxt_qplib_db_pacing_data *pacing_data;
};

static inline bool bnxt_qplib_is_chip_gen_p7(struct bnxt_qplib_chip_ctx *cctx)
{
	return (cctx->chip_num == CHIP_NUM_58818 ||
		cctx->chip_num == CHIP_NUM_57608);
}

static inline bool bnxt_qplib_is_chip_gen_p5(struct bnxt_qplib_chip_ctx *cctx)
{
	return (cctx->chip_num == CHIP_NUM_57508 ||
		cctx->chip_num == CHIP_NUM_57504 ||
		cctx->chip_num == CHIP_NUM_57502);
}

static inline bool bnxt_qplib_is_chip_gen_p5_p7(struct bnxt_qplib_chip_ctx *cctx)
{
	return bnxt_qplib_is_chip_gen_p5(cctx) || bnxt_qplib_is_chip_gen_p7(cctx);
}

static inline u8 bnxt_qplib_get_hwq_type(struct bnxt_qplib_res *res)
{
	return bnxt_qplib_is_chip_gen_p5_p7(res->cctx) ?
					HWQ_TYPE_QUEUE : HWQ_TYPE_L2_CMPL;
}

static inline u8 bnxt_qplib_get_ring_type(struct bnxt_qplib_chip_ctx *cctx)
{
	return bnxt_qplib_is_chip_gen_p5_p7(cctx) ?
	       RING_ALLOC_REQ_RING_TYPE_NQ :
	       RING_ALLOC_REQ_RING_TYPE_ROCE_CMPL;
}

static inline u8 bnxt_qplib_base_pg_size(struct bnxt_qplib_hwq *hwq)
{
	u8 pg_size = BNXT_QPLIB_HWRM_PG_SIZE_4K;
	struct bnxt_qplib_pbl *pbl;

	pbl = &hwq->pbl[PBL_LVL_0];
	switch (pbl->pg_size) {
	case ROCE_PG_SIZE_4K:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_4K;
		break;
	case ROCE_PG_SIZE_8K:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_8K;
		break;
	case ROCE_PG_SIZE_64K:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_64K;
		break;
	case ROCE_PG_SIZE_2M:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_2M;
		break;
	case ROCE_PG_SIZE_8M:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_8M;
		break;
	case ROCE_PG_SIZE_1G:
		pg_size = BNXT_QPLIB_HWRM_PG_SIZE_1G;
		break;
	default:
		break;
	}

	return pg_size;
}

static inline void *bnxt_qplib_get_qe(struct bnxt_qplib_hwq *hwq,
				      u32 indx, u64 *pg)
{
	u32 pg_num, pg_idx;

	pg_num = (indx / hwq->qe_ppg);
	pg_idx = (indx % hwq->qe_ppg);
	if (pg)
		*pg = (u64)&hwq->pbl_ptr[pg_num];
	return (void *)(hwq->pbl_ptr[pg_num] + hwq->element_size * pg_idx);
}

static inline void *bnxt_qplib_get_prod_qe(struct bnxt_qplib_hwq *hwq, u32 idx)
{
	idx += hwq->prod;
	if (idx >= hwq->depth)
		idx -= hwq->depth;
	return bnxt_qplib_get_qe(hwq, idx, NULL);
}

#define to_bnxt_qplib(ptr, type, member)	\
	container_of(ptr, type, member)

struct bnxt_qplib_pd;
struct bnxt_qplib_dev_attr;

void bnxt_qplib_free_hwq(struct bnxt_qplib_res *res,
			 struct bnxt_qplib_hwq *hwq);
int bnxt_qplib_alloc_init_hwq(struct bnxt_qplib_hwq *hwq,
			      struct bnxt_qplib_hwq_attr *hwq_attr);
int bnxt_qplib_alloc_pd(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pd *pd);
int bnxt_qplib_dealloc_pd(struct bnxt_qplib_res *res,
			  struct bnxt_qplib_pd_tbl *pd_tbl,
			  struct bnxt_qplib_pd *pd);
int bnxt_qplib_alloc_dpi(struct bnxt_qplib_res *res,
			 struct bnxt_qplib_dpi *dpi,
			 void *app, u8 type);
int bnxt_qplib_dealloc_dpi(struct bnxt_qplib_res *res,
			   struct bnxt_qplib_dpi *dpi);
void bnxt_qplib_cleanup_res(struct bnxt_qplib_res *res);
int bnxt_qplib_init_res(struct bnxt_qplib_res *res);
void bnxt_qplib_free_res(struct bnxt_qplib_res *res);
int bnxt_qplib_alloc_res(struct bnxt_qplib_res *res, struct net_device *netdev);
void bnxt_qplib_free_ctx(struct bnxt_qplib_res *res,
			 struct bnxt_qplib_ctx *ctx);
int bnxt_qplib_alloc_ctx(struct bnxt_qplib_res *res,
			 struct bnxt_qplib_ctx *ctx,
			 bool virt_fn, bool is_p5);
int bnxt_qplib_map_db_bar(struct bnxt_qplib_res *res);
void bnxt_qplib_unmap_db_bar(struct bnxt_qplib_res *res);

int bnxt_qplib_determine_atomics(struct pci_dev *dev);

static inline void bnxt_qplib_hwq_incr_prod(struct bnxt_qplib_db_info *dbinfo,
					    struct bnxt_qplib_hwq *hwq, u32 cnt)
{
	/* move prod and update toggle/epoch if wrap around */
	hwq->prod += cnt;
	if (hwq->prod >= hwq->depth) {
		hwq->prod %= hwq->depth;
		dbinfo->flags ^= 1UL << BNXT_QPLIB_FLAG_EPOCH_PROD_SHIFT;
	}
}

static inline void bnxt_qplib_hwq_incr_cons(u32 max_elements, u32 *cons, u32 cnt,
					    u32 *dbinfo_flags)
{
	/* move cons and update toggle/epoch if wrap around */
	*cons += cnt;
	if (*cons >= max_elements) {
		*cons %= max_elements;
		*dbinfo_flags ^= 1UL << BNXT_QPLIB_FLAG_EPOCH_CONS_SHIFT;
	}
}

static inline void bnxt_qplib_ring_db32(struct bnxt_qplib_db_info *info,
					bool arm)
{
	u32 key = 0;

	key |= info->hwq->cons | (CMPL_DOORBELL_IDX_VALID |
		(CMPL_DOORBELL_KEY_CMPL & CMPL_DOORBELL_KEY_MASK));
	if (!arm)
		key |= CMPL_DOORBELL_MASK;
	writel(key, info->db);
}

#define BNXT_QPLIB_INIT_DBHDR(xid, type, indx, toggle) \
	(((u64)(((xid) & DBC_DBC_XID_MASK) | DBC_DBC_PATH_ROCE |  \
		(type) | BNXT_QPLIB_DBR_VALID) << 32) | (indx) |  \
	 (((u32)(toggle)) << (BNXT_QPLIB_DBR_TOGGLE_SHIFT)))

static inline void bnxt_qplib_ring_db(struct bnxt_qplib_db_info *info,
				      u32 type)
{
	u64 key = 0;
	u32 indx;
	u8 toggle = 0;

	if (type == DBC_DBC_TYPE_CQ_ARMALL ||
	    type == DBC_DBC_TYPE_CQ_ARMSE)
		toggle = info->toggle;

	indx = (info->hwq->cons & DBC_DBC_INDEX_MASK) |
	       ((info->flags & BNXT_QPLIB_FLAG_EPOCH_CONS_MASK) <<
		 BNXT_QPLIB_DB_EPOCH_CONS_SHIFT);

	key =  BNXT_QPLIB_INIT_DBHDR(info->xid, type, indx, toggle);
	writeq(key, info->db);
}

static inline void bnxt_qplib_ring_prod_db(struct bnxt_qplib_db_info *info,
					   u32 type)
{
	u64 key = 0;
	u32 indx;

	indx = (((info->hwq->prod / info->max_slot) & DBC_DBC_INDEX_MASK) |
		((info->flags & BNXT_QPLIB_FLAG_EPOCH_PROD_MASK) <<
		 BNXT_QPLIB_DB_EPOCH_PROD_SHIFT));
	key = BNXT_QPLIB_INIT_DBHDR(info->xid, type, indx, 0);
	writeq(key, info->db);
}

static inline void bnxt_qplib_armen_db(struct bnxt_qplib_db_info *info,
				       u32 type)
{
	u64 key = 0;
	u8 toggle = 0;

	if (type == DBC_DBC_TYPE_CQ_ARMENA || type == DBC_DBC_TYPE_SRQ_ARMENA)
		toggle = info->toggle;
	/* Index always at 0 */
	key = BNXT_QPLIB_INIT_DBHDR(info->xid, type, 0, toggle);
	writeq(key, info->priv_db);
}

static inline void bnxt_qplib_srq_arm_db(struct bnxt_qplib_db_info *info,
					 u32 th)
{
	u64 key = 0;

	key = BNXT_QPLIB_INIT_DBHDR(info->xid, DBC_DBC_TYPE_SRQ_ARM, th, info->toggle);
	writeq(key, info->priv_db);
}

static inline void bnxt_qplib_ring_nq_db(struct bnxt_qplib_db_info *info,
					 struct bnxt_qplib_chip_ctx *cctx,
					 bool arm)
{
	u32 type;

	type = arm ? DBC_DBC_TYPE_NQ_ARM : DBC_DBC_TYPE_NQ;
	if (bnxt_qplib_is_chip_gen_p5_p7(cctx))
		bnxt_qplib_ring_db(info, type);
	else
		bnxt_qplib_ring_db32(info, arm);
}

static inline bool _is_ext_stats_supported(u16 dev_cap_flags)
{
	return dev_cap_flags &
		CREQ_QUERY_FUNC_RESP_SB_EXT_STATS;
}

static inline int bnxt_ext_stats_supported(struct bnxt_qplib_chip_ctx *ctx,
					   u16 flags, bool virtfn)
{
	/* ext stats supported if cap flag is set AND is a PF OR a Thor2 VF */
	return (_is_ext_stats_supported(flags) &&
		((virtfn && bnxt_qplib_is_chip_gen_p7(ctx)) || (!virtfn)));
}

static inline bool _is_hw_retx_supported(u16 dev_cap_flags)
{
	return dev_cap_flags &
		(CREQ_QUERY_FUNC_RESP_SB_HW_REQUESTER_RETX_ENABLED |
		 CREQ_QUERY_FUNC_RESP_SB_HW_RESPONDER_RETX_ENABLED);
}

#define BNXT_RE_HW_RETX(a) _is_hw_retx_supported((a))

static inline bool _is_host_msn_table(u16 dev_cap_ext_flags2)
{
	return (dev_cap_ext_flags2 & CREQ_QUERY_FUNC_RESP_SB_REQ_RETRANSMISSION_SUPPORT_MASK) ==
		CREQ_QUERY_FUNC_RESP_SB_REQ_RETRANSMISSION_SUPPORT_HOST_MSN_TABLE;
}

static inline u8 bnxt_qplib_dbr_pacing_en(struct bnxt_qplib_chip_ctx *cctx)
{
	return cctx->modes.dbr_pacing;
}

static inline bool _is_alloc_mr_unified(u16 dev_cap_flags)
{
	return dev_cap_flags & CREQ_QUERY_FUNC_RESP_SB_MR_REGISTER_ALLOC;
}

static inline bool _is_relaxed_ordering_supported(u16 dev_cap_ext_flags2)
{
	return dev_cap_ext_flags2 & CREQ_QUERY_FUNC_RESP_SB_MEMORY_REGION_RO_SUPPORTED;
}

static inline bool _is_optimize_modify_qp_supported(u16 dev_cap_ext_flags2)
{
	return dev_cap_ext_flags2 & CREQ_QUERY_FUNC_RESP_SB_OPTIMIZE_MODIFY_QP_SUPPORTED;
}

static inline bool _is_min_rnr_in_rtr_rts_mandatory(u16 dev_cap_ext_flags2)
{
	return !!(dev_cap_ext_flags2 & CREQ_QUERY_FUNC_RESP_SB_MIN_RNR_RTR_RTS_OPT_SUPPORTED);
}

static inline bool _is_cq_coalescing_supported(u16 dev_cap_ext_flags2)
{
	return dev_cap_ext_flags2 & CREQ_QUERY_FUNC_RESP_SB_CQ_COALESCING_SUPPORTED;
}

static inline bool _is_max_srq_ext_supported(u16 dev_cap_ext_flags_2)
{
	return !!(dev_cap_ext_flags_2 & CREQ_QUERY_FUNC_RESP_SB_MAX_SRQ_EXTENDED);
}

#endif /* __BNXT_QPLIB_RES_H__ */
