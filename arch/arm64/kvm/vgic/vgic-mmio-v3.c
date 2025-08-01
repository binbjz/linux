// SPDX-License-Identifier: GPL-2.0-only
/*
 * VGICv3 MMIO handling functions
 */

#include <linux/bitfield.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <linux/interrupt.h>
#include <kvm/iodev.h>
#include <kvm/arm_vgic.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_mmu.h>

#include "vgic.h"
#include "vgic-mmio.h"

/* extract @num bytes at @offset bytes offset in data */
unsigned long extract_bytes(u64 data, unsigned int offset,
			    unsigned int num)
{
	return (data >> (offset * 8)) & GENMASK_ULL(num * 8 - 1, 0);
}

/* allows updates of any half of a 64-bit register (or the whole thing) */
u64 update_64bit_reg(u64 reg, unsigned int offset, unsigned int len,
		     unsigned long val)
{
	int lower = (offset & 4) * 8;
	int upper = lower + 8 * len - 1;

	reg &= ~GENMASK_ULL(upper, lower);
	val &= GENMASK_ULL(len * 8 - 1, 0);

	return reg | ((u64)val << lower);
}

bool vgic_has_its(struct kvm *kvm)
{
	struct vgic_dist *dist = &kvm->arch.vgic;

	if (dist->vgic_model != KVM_DEV_TYPE_ARM_VGIC_V3)
		return false;

	return dist->has_its;
}

bool vgic_supports_direct_msis(struct kvm *kvm)
{
	return kvm_vgic_global_state.has_gicv4 && vgic_has_its(kvm);
}

bool system_supports_direct_sgis(void)
{
	return kvm_vgic_global_state.has_gicv4_1 && gic_cpuif_has_vsgi();
}

bool vgic_supports_direct_sgis(struct kvm *kvm)
{
	return kvm->arch.vgic.nassgicap;
}

/*
 * The Revision field in the IIDR have the following meanings:
 *
 * Revision 2: Interrupt groups are guest-configurable and signaled using
 * 	       their configured groups.
 */

static unsigned long vgic_mmio_read_v3_misc(struct kvm_vcpu *vcpu,
					    gpa_t addr, unsigned int len)
{
	struct vgic_dist *vgic = &vcpu->kvm->arch.vgic;
	u32 value = 0;

	switch (addr & 0x0c) {
	case GICD_CTLR:
		if (vgic->enabled)
			value |= GICD_CTLR_ENABLE_SS_G1;
		value |= GICD_CTLR_ARE_NS | GICD_CTLR_DS;
		if (vgic->nassgireq)
			value |= GICD_CTLR_nASSGIreq;
		break;
	case GICD_TYPER:
		value = vgic->nr_spis + VGIC_NR_PRIVATE_IRQS;
		value = (value >> 5) - 1;
		if (vgic_has_its(vcpu->kvm)) {
			value |= (INTERRUPT_ID_BITS_ITS - 1) << 19;
			value |= GICD_TYPER_LPIS;
		} else {
			value |= (INTERRUPT_ID_BITS_SPIS - 1) << 19;
		}
		break;
	case GICD_TYPER2:
		if (vgic_supports_direct_sgis(vcpu->kvm))
			value = GICD_TYPER2_nASSGIcap;
		break;
	case GICD_IIDR:
		value = (PRODUCT_ID_KVM << GICD_IIDR_PRODUCT_ID_SHIFT) |
			(vgic->implementation_rev << GICD_IIDR_REVISION_SHIFT) |
			(IMPLEMENTER_ARM << GICD_IIDR_IMPLEMENTER_SHIFT);
		break;
	default:
		return 0;
	}

	return value;
}

static void vgic_mmio_write_v3_misc(struct kvm_vcpu *vcpu,
				    gpa_t addr, unsigned int len,
				    unsigned long val)
{
	struct vgic_dist *dist = &vcpu->kvm->arch.vgic;

	switch (addr & 0x0c) {
	case GICD_CTLR: {
		bool was_enabled, is_hwsgi;

		mutex_lock(&vcpu->kvm->arch.config_lock);

		was_enabled = dist->enabled;
		is_hwsgi = dist->nassgireq;

		dist->enabled = val & GICD_CTLR_ENABLE_SS_G1;

		/* Not a GICv4.1? No HW SGIs */
		if (!vgic_supports_direct_sgis(vcpu->kvm))
			val &= ~GICD_CTLR_nASSGIreq;

		/* Dist stays enabled? nASSGIreq is RO */
		if (was_enabled && dist->enabled) {
			val &= ~GICD_CTLR_nASSGIreq;
			val |= FIELD_PREP(GICD_CTLR_nASSGIreq, is_hwsgi);
		}

		/* Switching HW SGIs? */
		dist->nassgireq = val & GICD_CTLR_nASSGIreq;
		if (is_hwsgi != dist->nassgireq)
			vgic_v4_configure_vsgis(vcpu->kvm);

		if (vgic_supports_direct_sgis(vcpu->kvm) &&
		    was_enabled != dist->enabled)
			kvm_make_all_cpus_request(vcpu->kvm, KVM_REQ_RELOAD_GICv4);
		else if (!was_enabled && dist->enabled)
			vgic_kick_vcpus(vcpu->kvm);

		mutex_unlock(&vcpu->kvm->arch.config_lock);
		break;
	}
	case GICD_TYPER:
	case GICD_TYPER2:
	case GICD_IIDR:
		/* This is at best for documentation purposes... */
		return;
	}
}

static int vgic_mmio_uaccess_write_v3_misc(struct kvm_vcpu *vcpu,
					   gpa_t addr, unsigned int len,
					   unsigned long val)
{
	struct vgic_dist *dist = &vcpu->kvm->arch.vgic;
	u32 reg;

	switch (addr & 0x0c) {
	case GICD_TYPER2:
		reg = vgic_mmio_read_v3_misc(vcpu, addr, len);

		if (reg == val)
			return 0;
		if (vgic_initialized(vcpu->kvm))
			return -EBUSY;
		if ((reg ^ val) & ~GICD_TYPER2_nASSGIcap)
			return -EINVAL;
		if (!system_supports_direct_sgis() && val)
			return -EINVAL;

		dist->nassgicap = val & GICD_TYPER2_nASSGIcap;
		return 0;
	case GICD_IIDR:
		reg = vgic_mmio_read_v3_misc(vcpu, addr, len);
		if ((reg ^ val) & ~GICD_IIDR_REVISION_MASK)
			return -EINVAL;

		reg = FIELD_GET(GICD_IIDR_REVISION_MASK, reg);
		switch (reg) {
		case KVM_VGIC_IMP_REV_2:
		case KVM_VGIC_IMP_REV_3:
			dist->implementation_rev = reg;
			return 0;
		default:
			return -EINVAL;
		}
	case GICD_CTLR:
		/* Not a GICv4.1? No HW SGIs */
		if (!vgic_supports_direct_sgis(vcpu->kvm))
			val &= ~GICD_CTLR_nASSGIreq;

		dist->enabled = val & GICD_CTLR_ENABLE_SS_G1;
		dist->nassgireq = val & GICD_CTLR_nASSGIreq;
		return 0;
	}

	vgic_mmio_write_v3_misc(vcpu, addr, len, val);
	return 0;
}

static unsigned long vgic_mmio_read_irouter(struct kvm_vcpu *vcpu,
					    gpa_t addr, unsigned int len)
{
	int intid = VGIC_ADDR_TO_INTID(addr, 64);
	struct vgic_irq *irq = vgic_get_irq(vcpu->kvm, intid);
	unsigned long ret = 0;

	if (!irq)
		return 0;

	/* The upper word is RAZ for us. */
	if (!(addr & 4))
		ret = extract_bytes(READ_ONCE(irq->mpidr), addr & 7, len);

	vgic_put_irq(vcpu->kvm, irq);
	return ret;
}

static void vgic_mmio_write_irouter(struct kvm_vcpu *vcpu,
				    gpa_t addr, unsigned int len,
				    unsigned long val)
{
	int intid = VGIC_ADDR_TO_INTID(addr, 64);
	struct vgic_irq *irq;
	unsigned long flags;

	/* The upper word is WI for us since we don't implement Aff3. */
	if (addr & 4)
		return;

	irq = vgic_get_irq(vcpu->kvm, intid);

	if (!irq)
		return;

	raw_spin_lock_irqsave(&irq->irq_lock, flags);

	/* We only care about and preserve Aff0, Aff1 and Aff2. */
	irq->mpidr = val & GENMASK(23, 0);
	irq->target_vcpu = kvm_mpidr_to_vcpu(vcpu->kvm, irq->mpidr);

	raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
	vgic_put_irq(vcpu->kvm, irq);
}

bool vgic_lpis_enabled(struct kvm_vcpu *vcpu)
{
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;

	return atomic_read(&vgic_cpu->ctlr) == GICR_CTLR_ENABLE_LPIS;
}

static unsigned long vgic_mmio_read_v3r_ctlr(struct kvm_vcpu *vcpu,
					     gpa_t addr, unsigned int len)
{
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	unsigned long val;

	val = atomic_read(&vgic_cpu->ctlr);
	if (vgic_get_implementation_rev(vcpu) >= KVM_VGIC_IMP_REV_3)
		val |= GICR_CTLR_IR | GICR_CTLR_CES;

	return val;
}

static void vgic_mmio_write_v3r_ctlr(struct kvm_vcpu *vcpu,
				     gpa_t addr, unsigned int len,
				     unsigned long val)
{
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	u32 ctlr;

	if (!vgic_has_its(vcpu->kvm))
		return;

	if (!(val & GICR_CTLR_ENABLE_LPIS)) {
		/*
		 * Don't disable if RWP is set, as there already an
		 * ongoing disable. Funky guest...
		 */
		ctlr = atomic_cmpxchg_acquire(&vgic_cpu->ctlr,
					      GICR_CTLR_ENABLE_LPIS,
					      GICR_CTLR_RWP);
		if (ctlr != GICR_CTLR_ENABLE_LPIS)
			return;

		vgic_flush_pending_lpis(vcpu);
		vgic_its_invalidate_all_caches(vcpu->kvm);
		atomic_set_release(&vgic_cpu->ctlr, 0);
	} else {
		ctlr = atomic_cmpxchg_acquire(&vgic_cpu->ctlr, 0,
					      GICR_CTLR_ENABLE_LPIS);
		if (ctlr != 0)
			return;

		vgic_enable_lpis(vcpu);
	}
}

static bool vgic_mmio_vcpu_rdist_is_last(struct kvm_vcpu *vcpu)
{
	struct vgic_dist *vgic = &vcpu->kvm->arch.vgic;
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	struct vgic_redist_region *iter, *rdreg = vgic_cpu->rdreg;

	if (!rdreg)
		return false;

	if (vgic_cpu->rdreg_index < rdreg->free_index - 1) {
		return false;
	} else if (rdreg->count && vgic_cpu->rdreg_index == (rdreg->count - 1)) {
		struct list_head *rd_regions = &vgic->rd_regions;
		gpa_t end = rdreg->base + rdreg->count * KVM_VGIC_V3_REDIST_SIZE;

		/*
		 * the rdist is the last one of the redist region,
		 * check whether there is no other contiguous rdist region
		 */
		list_for_each_entry(iter, rd_regions, list) {
			if (iter->base == end && iter->free_index > 0)
				return false;
		}
	}
	return true;
}

static unsigned long vgic_mmio_read_v3r_typer(struct kvm_vcpu *vcpu,
					      gpa_t addr, unsigned int len)
{
	unsigned long mpidr = kvm_vcpu_get_mpidr_aff(vcpu);
	int target_vcpu_id = vcpu->vcpu_id;
	u64 value;

	value = (u64)(mpidr & GENMASK(23, 0)) << 32;
	value |= ((target_vcpu_id & 0xffff) << 8);

	if (vgic_has_its(vcpu->kvm))
		value |= GICR_TYPER_PLPIS;

	if (vgic_mmio_vcpu_rdist_is_last(vcpu))
		value |= GICR_TYPER_LAST;

	return extract_bytes(value, addr & 7, len);
}

static unsigned long vgic_mmio_read_v3r_iidr(struct kvm_vcpu *vcpu,
					     gpa_t addr, unsigned int len)
{
	return (PRODUCT_ID_KVM << 24) | (IMPLEMENTER_ARM << 0);
}

static unsigned long vgic_mmio_read_v3_idregs(struct kvm_vcpu *vcpu,
					      gpa_t addr, unsigned int len)
{
	switch (addr & 0xffff) {
	case GICD_PIDR2:
		/* report a GICv3 compliant implementation */
		return 0x3b;
	}

	return 0;
}

static int vgic_v3_uaccess_write_pending(struct kvm_vcpu *vcpu,
					 gpa_t addr, unsigned int len,
					 unsigned long val)
{
	int ret;

	ret = vgic_uaccess_write_spending(vcpu, addr, len, val);
	if (ret)
		return ret;

	return vgic_uaccess_write_cpending(vcpu, addr, len, ~val);
}

/* We want to avoid outer shareable. */
u64 vgic_sanitise_shareability(u64 field)
{
	switch (field) {
	case GIC_BASER_OuterShareable:
		return GIC_BASER_InnerShareable;
	default:
		return field;
	}
}

/* Avoid any inner non-cacheable mapping. */
u64 vgic_sanitise_inner_cacheability(u64 field)
{
	switch (field) {
	case GIC_BASER_CACHE_nCnB:
	case GIC_BASER_CACHE_nC:
		return GIC_BASER_CACHE_RaWb;
	default:
		return field;
	}
}

/* Non-cacheable or same-as-inner are OK. */
u64 vgic_sanitise_outer_cacheability(u64 field)
{
	switch (field) {
	case GIC_BASER_CACHE_SameAsInner:
	case GIC_BASER_CACHE_nC:
		return field;
	default:
		return GIC_BASER_CACHE_SameAsInner;
	}
}

u64 vgic_sanitise_field(u64 reg, u64 field_mask, int field_shift,
			u64 (*sanitise_fn)(u64))
{
	u64 field = (reg & field_mask) >> field_shift;

	field = sanitise_fn(field) << field_shift;
	return (reg & ~field_mask) | field;
}

#define PROPBASER_RES0_MASK						\
	(GENMASK_ULL(63, 59) | GENMASK_ULL(55, 52) | GENMASK_ULL(6, 5))
#define PENDBASER_RES0_MASK						\
	(BIT_ULL(63) | GENMASK_ULL(61, 59) | GENMASK_ULL(55, 52) |	\
	 GENMASK_ULL(15, 12) | GENMASK_ULL(6, 0))

static u64 vgic_sanitise_pendbaser(u64 reg)
{
	reg = vgic_sanitise_field(reg, GICR_PENDBASER_SHAREABILITY_MASK,
				  GICR_PENDBASER_SHAREABILITY_SHIFT,
				  vgic_sanitise_shareability);
	reg = vgic_sanitise_field(reg, GICR_PENDBASER_INNER_CACHEABILITY_MASK,
				  GICR_PENDBASER_INNER_CACHEABILITY_SHIFT,
				  vgic_sanitise_inner_cacheability);
	reg = vgic_sanitise_field(reg, GICR_PENDBASER_OUTER_CACHEABILITY_MASK,
				  GICR_PENDBASER_OUTER_CACHEABILITY_SHIFT,
				  vgic_sanitise_outer_cacheability);

	reg &= ~PENDBASER_RES0_MASK;

	return reg;
}

static u64 vgic_sanitise_propbaser(u64 reg)
{
	reg = vgic_sanitise_field(reg, GICR_PROPBASER_SHAREABILITY_MASK,
				  GICR_PROPBASER_SHAREABILITY_SHIFT,
				  vgic_sanitise_shareability);
	reg = vgic_sanitise_field(reg, GICR_PROPBASER_INNER_CACHEABILITY_MASK,
				  GICR_PROPBASER_INNER_CACHEABILITY_SHIFT,
				  vgic_sanitise_inner_cacheability);
	reg = vgic_sanitise_field(reg, GICR_PROPBASER_OUTER_CACHEABILITY_MASK,
				  GICR_PROPBASER_OUTER_CACHEABILITY_SHIFT,
				  vgic_sanitise_outer_cacheability);

	reg &= ~PROPBASER_RES0_MASK;
	return reg;
}

static unsigned long vgic_mmio_read_propbase(struct kvm_vcpu *vcpu,
					     gpa_t addr, unsigned int len)
{
	struct vgic_dist *dist = &vcpu->kvm->arch.vgic;

	return extract_bytes(dist->propbaser, addr & 7, len);
}

static void vgic_mmio_write_propbase(struct kvm_vcpu *vcpu,
				     gpa_t addr, unsigned int len,
				     unsigned long val)
{
	struct vgic_dist *dist = &vcpu->kvm->arch.vgic;
	u64 old_propbaser, propbaser;

	/* Storing a value with LPIs already enabled is undefined */
	if (vgic_lpis_enabled(vcpu))
		return;

	do {
		old_propbaser = READ_ONCE(dist->propbaser);
		propbaser = old_propbaser;
		propbaser = update_64bit_reg(propbaser, addr & 4, len, val);
		propbaser = vgic_sanitise_propbaser(propbaser);
	} while (cmpxchg64(&dist->propbaser, old_propbaser,
			   propbaser) != old_propbaser);
}

static unsigned long vgic_mmio_read_pendbase(struct kvm_vcpu *vcpu,
					     gpa_t addr, unsigned int len)
{
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	u64 value = vgic_cpu->pendbaser;

	value &= ~GICR_PENDBASER_PTZ;

	return extract_bytes(value, addr & 7, len);
}

static void vgic_mmio_write_pendbase(struct kvm_vcpu *vcpu,
				     gpa_t addr, unsigned int len,
				     unsigned long val)
{
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	u64 old_pendbaser, pendbaser;

	/* Storing a value with LPIs already enabled is undefined */
	if (vgic_lpis_enabled(vcpu))
		return;

	do {
		old_pendbaser = READ_ONCE(vgic_cpu->pendbaser);
		pendbaser = old_pendbaser;
		pendbaser = update_64bit_reg(pendbaser, addr & 4, len, val);
		pendbaser = vgic_sanitise_pendbaser(pendbaser);
	} while (cmpxchg64(&vgic_cpu->pendbaser, old_pendbaser,
			   pendbaser) != old_pendbaser);
}

static unsigned long vgic_mmio_read_sync(struct kvm_vcpu *vcpu,
					 gpa_t addr, unsigned int len)
{
	return !!atomic_read(&vcpu->arch.vgic_cpu.syncr_busy);
}

static void vgic_set_rdist_busy(struct kvm_vcpu *vcpu, bool busy)
{
	if (busy) {
		atomic_inc(&vcpu->arch.vgic_cpu.syncr_busy);
		smp_mb__after_atomic();
	} else {
		smp_mb__before_atomic();
		atomic_dec(&vcpu->arch.vgic_cpu.syncr_busy);
	}
}

static void vgic_mmio_write_invlpi(struct kvm_vcpu *vcpu,
				   gpa_t addr, unsigned int len,
				   unsigned long val)
{
	struct vgic_irq *irq;
	u32 intid;

	/*
	 * If the guest wrote only to the upper 32bit part of the
	 * register, drop the write on the floor, as it is only for
	 * vPEs (which we don't support for obvious reasons).
	 *
	 * Also discard the access if LPIs are not enabled.
	 */
	if ((addr & 4) || !vgic_lpis_enabled(vcpu))
		return;

	intid = lower_32_bits(val);
	if (intid < VGIC_MIN_LPI)
		return;

	vgic_set_rdist_busy(vcpu, true);

	irq = vgic_get_irq(vcpu->kvm, intid);
	if (irq) {
		vgic_its_inv_lpi(vcpu->kvm, irq);
		vgic_put_irq(vcpu->kvm, irq);
	}

	vgic_set_rdist_busy(vcpu, false);
}

static void vgic_mmio_write_invall(struct kvm_vcpu *vcpu,
				   gpa_t addr, unsigned int len,
				   unsigned long val)
{
	/* See vgic_mmio_write_invlpi() for the early return rationale */
	if ((addr & 4) || !vgic_lpis_enabled(vcpu))
		return;

	vgic_set_rdist_busy(vcpu, true);
	vgic_its_invall(vcpu);
	vgic_set_rdist_busy(vcpu, false);
}

/*
 * The GICv3 per-IRQ registers are split to control PPIs and SGIs in the
 * redistributors, while SPIs are covered by registers in the distributor
 * block. Trying to set private IRQs in this block gets ignored.
 * We take some special care here to fix the calculation of the register
 * offset.
 */
#define REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(off, rd, wr, ur, uw, bpi, acc) \
	{								\
		.reg_offset = off,					\
		.bits_per_irq = bpi,					\
		.len = (bpi * VGIC_NR_PRIVATE_IRQS) / 8,		\
		.access_flags = acc,					\
		.read = vgic_mmio_read_raz,				\
		.write = vgic_mmio_write_wi,				\
	}, {								\
		.reg_offset = off + (bpi * VGIC_NR_PRIVATE_IRQS) / 8,	\
		.bits_per_irq = bpi,					\
		.len = (bpi * (1024 - VGIC_NR_PRIVATE_IRQS)) / 8,	\
		.access_flags = acc,					\
		.read = rd,						\
		.write = wr,						\
		.uaccess_read = ur,					\
		.uaccess_write = uw,					\
	}

static const struct vgic_register_region vgic_v3_dist_registers[] = {
	REGISTER_DESC_WITH_LENGTH_UACCESS(GICD_CTLR,
		vgic_mmio_read_v3_misc, vgic_mmio_write_v3_misc,
		NULL, vgic_mmio_uaccess_write_v3_misc,
		16, VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICD_STATUSR,
		vgic_mmio_read_rao, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_IGROUPR,
		vgic_mmio_read_group, vgic_mmio_write_group, NULL, NULL, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ISENABLER,
		vgic_mmio_read_enable, vgic_mmio_write_senable,
		NULL, vgic_uaccess_write_senable, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ICENABLER,
		vgic_mmio_read_enable, vgic_mmio_write_cenable,
	       NULL, vgic_uaccess_write_cenable, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ISPENDR,
		vgic_mmio_read_pending, vgic_mmio_write_spending,
		vgic_uaccess_read_pending, vgic_v3_uaccess_write_pending, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ICPENDR,
		vgic_mmio_read_pending, vgic_mmio_write_cpending,
		vgic_mmio_read_raz, vgic_mmio_uaccess_write_wi, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ISACTIVER,
		vgic_mmio_read_active, vgic_mmio_write_sactive,
		vgic_uaccess_read_active, vgic_mmio_uaccess_write_sactive, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ICACTIVER,
		vgic_mmio_read_active, vgic_mmio_write_cactive,
		vgic_uaccess_read_active, vgic_mmio_uaccess_write_cactive,
		1, VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_IPRIORITYR,
		vgic_mmio_read_priority, vgic_mmio_write_priority, NULL, NULL,
		8, VGIC_ACCESS_32bit | VGIC_ACCESS_8bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ITARGETSR,
		vgic_mmio_read_raz, vgic_mmio_write_wi, NULL, NULL, 8,
		VGIC_ACCESS_32bit | VGIC_ACCESS_8bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_ICFGR,
		vgic_mmio_read_config, vgic_mmio_write_config, NULL, NULL, 2,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_IGRPMODR,
		vgic_mmio_read_raz, vgic_mmio_write_wi, NULL, NULL, 1,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_BITS_PER_IRQ_SHARED(GICD_IROUTER,
		vgic_mmio_read_irouter, vgic_mmio_write_irouter, NULL, NULL, 64,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICD_IDREGS,
		vgic_mmio_read_v3_idregs, vgic_mmio_write_wi, 48,
		VGIC_ACCESS_32bit),
};

static const struct vgic_register_region vgic_v3_rd_registers[] = {
	/* RD_base registers */
	REGISTER_DESC_WITH_LENGTH(GICR_CTLR,
		vgic_mmio_read_v3r_ctlr, vgic_mmio_write_v3r_ctlr, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_STATUSR,
		vgic_mmio_read_raz, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_IIDR,
		vgic_mmio_read_v3r_iidr, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(GICR_TYPER,
		vgic_mmio_read_v3r_typer, vgic_mmio_write_wi,
		NULL, vgic_mmio_uaccess_write_wi, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_WAKER,
		vgic_mmio_read_raz, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_PROPBASER,
		vgic_mmio_read_propbase, vgic_mmio_write_propbase, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_PENDBASER,
		vgic_mmio_read_pendbase, vgic_mmio_write_pendbase, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_INVLPIR,
		vgic_mmio_read_raz, vgic_mmio_write_invlpi, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_INVALLR,
		vgic_mmio_read_raz, vgic_mmio_write_invall, 8,
		VGIC_ACCESS_64bit | VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_SYNCR,
		vgic_mmio_read_sync, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(GICR_IDREGS,
		vgic_mmio_read_v3_idregs, vgic_mmio_write_wi, 48,
		VGIC_ACCESS_32bit),
	/* SGI_base registers */
	REGISTER_DESC_WITH_LENGTH(SZ_64K + GICR_IGROUPR0,
		vgic_mmio_read_group, vgic_mmio_write_group, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ISENABLER0,
		vgic_mmio_read_enable, vgic_mmio_write_senable,
		NULL, vgic_uaccess_write_senable, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ICENABLER0,
		vgic_mmio_read_enable, vgic_mmio_write_cenable,
		NULL, vgic_uaccess_write_cenable, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ISPENDR0,
		vgic_mmio_read_pending, vgic_mmio_write_spending,
		vgic_uaccess_read_pending, vgic_v3_uaccess_write_pending, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ICPENDR0,
		vgic_mmio_read_pending, vgic_mmio_write_cpending,
		vgic_mmio_read_raz, vgic_mmio_uaccess_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ISACTIVER0,
		vgic_mmio_read_active, vgic_mmio_write_sactive,
		vgic_uaccess_read_active, vgic_mmio_uaccess_write_sactive, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH_UACCESS(SZ_64K + GICR_ICACTIVER0,
		vgic_mmio_read_active, vgic_mmio_write_cactive,
		vgic_uaccess_read_active, vgic_mmio_uaccess_write_cactive, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(SZ_64K + GICR_IPRIORITYR0,
		vgic_mmio_read_priority, vgic_mmio_write_priority, 32,
		VGIC_ACCESS_32bit | VGIC_ACCESS_8bit),
	REGISTER_DESC_WITH_LENGTH(SZ_64K + GICR_ICFGR0,
		vgic_mmio_read_config, vgic_mmio_write_config, 8,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(SZ_64K + GICR_IGRPMODR0,
		vgic_mmio_read_raz, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
	REGISTER_DESC_WITH_LENGTH(SZ_64K + GICR_NSACR,
		vgic_mmio_read_raz, vgic_mmio_write_wi, 4,
		VGIC_ACCESS_32bit),
};

unsigned int vgic_v3_init_dist_iodev(struct vgic_io_device *dev)
{
	dev->regions = vgic_v3_dist_registers;
	dev->nr_regions = ARRAY_SIZE(vgic_v3_dist_registers);

	kvm_iodevice_init(&dev->dev, &kvm_io_gic_ops);

	return SZ_64K;
}

/**
 * vgic_register_redist_iodev - register a single redist iodev
 * @vcpu:    The VCPU to which the redistributor belongs
 *
 * Register a KVM iodev for this VCPU's redistributor using the address
 * provided.
 *
 * Return 0 on success, -ERRNO otherwise.
 */
int vgic_register_redist_iodev(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct vgic_dist *vgic = &kvm->arch.vgic;
	struct vgic_cpu *vgic_cpu = &vcpu->arch.vgic_cpu;
	struct vgic_io_device *rd_dev = &vcpu->arch.vgic_cpu.rd_iodev;
	struct vgic_redist_region *rdreg;
	gpa_t rd_base;
	int ret = 0;

	lockdep_assert_held(&kvm->slots_lock);
	mutex_lock(&kvm->arch.config_lock);

	if (!IS_VGIC_ADDR_UNDEF(vgic_cpu->rd_iodev.base_addr))
		goto out_unlock;

	/*
	 * We may be creating VCPUs before having set the base address for the
	 * redistributor region, in which case we will come back to this
	 * function for all VCPUs when the base address is set.  Just return
	 * without doing any work for now.
	 */
	rdreg = vgic_v3_rdist_free_slot(&vgic->rd_regions);
	if (!rdreg)
		goto out_unlock;

	if (!vgic_v3_check_base(kvm)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vgic_cpu->rdreg = rdreg;
	vgic_cpu->rdreg_index = rdreg->free_index;

	rd_base = rdreg->base + rdreg->free_index * KVM_VGIC_V3_REDIST_SIZE;

	kvm_iodevice_init(&rd_dev->dev, &kvm_io_gic_ops);
	rd_dev->base_addr = rd_base;
	rd_dev->iodev_type = IODEV_REDIST;
	rd_dev->regions = vgic_v3_rd_registers;
	rd_dev->nr_regions = ARRAY_SIZE(vgic_v3_rd_registers);
	rd_dev->redist_vcpu = vcpu;

	mutex_unlock(&kvm->arch.config_lock);

	ret = kvm_io_bus_register_dev(kvm, KVM_MMIO_BUS, rd_base,
				      2 * SZ_64K, &rd_dev->dev);
	if (ret)
		return ret;

	/* Protected by slots_lock */
	rdreg->free_index++;
	return 0;

out_unlock:
	mutex_unlock(&kvm->arch.config_lock);
	return ret;
}

void vgic_unregister_redist_iodev(struct kvm_vcpu *vcpu)
{
	struct vgic_io_device *rd_dev = &vcpu->arch.vgic_cpu.rd_iodev;

	kvm_io_bus_unregister_dev(vcpu->kvm, KVM_MMIO_BUS, &rd_dev->dev);
}

static int vgic_register_all_redist_iodevs(struct kvm *kvm)
{
	struct kvm_vcpu *vcpu;
	unsigned long c;
	int ret = 0;

	lockdep_assert_held(&kvm->slots_lock);

	kvm_for_each_vcpu(c, vcpu, kvm) {
		ret = vgic_register_redist_iodev(vcpu);
		if (ret)
			break;
	}

	if (ret) {
		/* The current c failed, so iterate over the previous ones. */
		int i;

		for (i = 0; i < c; i++) {
			vcpu = kvm_get_vcpu(kvm, i);
			vgic_unregister_redist_iodev(vcpu);
		}
	}

	return ret;
}

/**
 * vgic_v3_alloc_redist_region - Allocate a new redistributor region
 *
 * Performs various checks before inserting the rdist region in the list.
 * Those tests depend on whether the size of the rdist region is known
 * (ie. count != 0). The list is sorted by rdist region index.
 *
 * @kvm: kvm handle
 * @index: redist region index
 * @base: base of the new rdist region
 * @count: number of redistributors the region is made of (0 in the old style
 * single region, whose size is induced from the number of vcpus)
 *
 * Return 0 on success, < 0 otherwise
 */
static int vgic_v3_alloc_redist_region(struct kvm *kvm, uint32_t index,
				       gpa_t base, uint32_t count)
{
	struct vgic_dist *d = &kvm->arch.vgic;
	struct vgic_redist_region *rdreg;
	struct list_head *rd_regions = &d->rd_regions;
	int nr_vcpus = atomic_read(&kvm->online_vcpus);
	size_t size = count ? count * KVM_VGIC_V3_REDIST_SIZE
			    : nr_vcpus * KVM_VGIC_V3_REDIST_SIZE;
	int ret;

	/* cross the end of memory ? */
	if (base + size < base)
		return -EINVAL;

	if (list_empty(rd_regions)) {
		if (index != 0)
			return -EINVAL;
	} else {
		rdreg = list_last_entry(rd_regions,
					struct vgic_redist_region, list);

		/* Don't mix single region and discrete redist regions */
		if (!count && rdreg->count)
			return -EINVAL;

		if (!count)
			return -EEXIST;

		if (index != rdreg->index + 1)
			return -EINVAL;
	}

	/*
	 * For legacy single-region redistributor regions (!count),
	 * check that the redistributor region does not overlap with the
	 * distributor's address space.
	 */
	if (!count && !IS_VGIC_ADDR_UNDEF(d->vgic_dist_base) &&
		vgic_dist_overlap(kvm, base, size))
		return -EINVAL;

	/* collision with any other rdist region? */
	if (vgic_v3_rdist_overlap(kvm, base, size))
		return -EINVAL;

	rdreg = kzalloc(sizeof(*rdreg), GFP_KERNEL_ACCOUNT);
	if (!rdreg)
		return -ENOMEM;

	rdreg->base = VGIC_ADDR_UNDEF;

	ret = vgic_check_iorange(kvm, rdreg->base, base, SZ_64K, size);
	if (ret)
		goto free;

	rdreg->base = base;
	rdreg->count = count;
	rdreg->free_index = 0;
	rdreg->index = index;

	list_add_tail(&rdreg->list, rd_regions);
	return 0;
free:
	kfree(rdreg);
	return ret;
}

void vgic_v3_free_redist_region(struct kvm *kvm, struct vgic_redist_region *rdreg)
{
	struct kvm_vcpu *vcpu;
	unsigned long c;

	lockdep_assert_held(&kvm->arch.config_lock);

	/* Garbage collect the region */
	kvm_for_each_vcpu(c, vcpu, kvm) {
		if (vcpu->arch.vgic_cpu.rdreg == rdreg)
			vcpu->arch.vgic_cpu.rdreg = NULL;
	}

	list_del(&rdreg->list);
	kfree(rdreg);
}

int vgic_v3_set_redist_base(struct kvm *kvm, u32 index, u64 addr, u32 count)
{
	int ret;

	mutex_lock(&kvm->arch.config_lock);
	ret = vgic_v3_alloc_redist_region(kvm, index, addr, count);
	mutex_unlock(&kvm->arch.config_lock);
	if (ret)
		return ret;

	/*
	 * Register iodevs for each existing VCPU.  Adding more VCPUs
	 * afterwards will register the iodevs when needed.
	 */
	ret = vgic_register_all_redist_iodevs(kvm);
	if (ret) {
		struct vgic_redist_region *rdreg;

		mutex_lock(&kvm->arch.config_lock);
		rdreg = vgic_v3_rdist_region_from_index(kvm, index);
		vgic_v3_free_redist_region(kvm, rdreg);
		mutex_unlock(&kvm->arch.config_lock);
		return ret;
	}

	return 0;
}

int vgic_v3_has_attr_regs(struct kvm_device *dev, struct kvm_device_attr *attr)
{
	const struct vgic_register_region *region;
	struct vgic_io_device iodev;
	struct vgic_reg_attr reg_attr;
	struct kvm_vcpu *vcpu;
	gpa_t addr;
	int ret;

	ret = vgic_v3_parse_attr(dev, attr, &reg_attr);
	if (ret)
		return ret;

	vcpu = reg_attr.vcpu;
	addr = reg_attr.addr;

	switch (attr->group) {
	case KVM_DEV_ARM_VGIC_GRP_DIST_REGS:
		iodev.regions = vgic_v3_dist_registers;
		iodev.nr_regions = ARRAY_SIZE(vgic_v3_dist_registers);
		iodev.base_addr = 0;
		break;
	case KVM_DEV_ARM_VGIC_GRP_REDIST_REGS:{
		iodev.regions = vgic_v3_rd_registers;
		iodev.nr_regions = ARRAY_SIZE(vgic_v3_rd_registers);
		iodev.base_addr = 0;
		break;
	}
	case KVM_DEV_ARM_VGIC_GRP_CPU_SYSREGS:
		return vgic_v3_has_cpu_sysregs_attr(vcpu, attr);
	default:
		return -ENXIO;
	}

	/* We only support aligned 32-bit accesses. */
	if (addr & 3)
		return -ENXIO;

	region = vgic_get_mmio_region(vcpu, &iodev, addr, sizeof(u32));
	if (!region)
		return -ENXIO;

	return 0;
}

/*
 * The ICC_SGI* registers encode the affinity differently from the MPIDR,
 * so provide a wrapper to use the existing defines to isolate a certain
 * affinity level.
 */
#define SGI_AFFINITY_LEVEL(reg, level) \
	((((reg) & ICC_SGI1R_AFFINITY_## level ##_MASK) \
	>> ICC_SGI1R_AFFINITY_## level ##_SHIFT) << MPIDR_LEVEL_SHIFT(level))

static void vgic_v3_queue_sgi(struct kvm_vcpu *vcpu, u32 sgi, bool allow_group1)
{
	struct vgic_irq *irq = vgic_get_vcpu_irq(vcpu, sgi);
	unsigned long flags;

	raw_spin_lock_irqsave(&irq->irq_lock, flags);

	/*
	 * An access targeting Group0 SGIs can only generate
	 * those, while an access targeting Group1 SGIs can
	 * generate interrupts of either group.
	 */
	if (!irq->group || allow_group1) {
		if (!irq->hw) {
			irq->pending_latch = true;
			vgic_queue_irq_unlock(vcpu->kvm, irq, flags);
		} else {
			/* HW SGI? Ask the GIC to inject it */
			int err;
			err = irq_set_irqchip_state(irq->host_irq,
						    IRQCHIP_STATE_PENDING,
						    true);
			WARN_RATELIMIT(err, "IRQ %d", irq->host_irq);
			raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
		}
	} else {
		raw_spin_unlock_irqrestore(&irq->irq_lock, flags);
	}

	vgic_put_irq(vcpu->kvm, irq);
}

/**
 * vgic_v3_dispatch_sgi - handle SGI requests from VCPUs
 * @vcpu: The VCPU requesting a SGI
 * @reg: The value written into ICC_{ASGI1,SGI0,SGI1}R by that VCPU
 * @allow_group1: Does the sysreg access allow generation of G1 SGIs
 *
 * With GICv3 (and ARE=1) CPUs trigger SGIs by writing to a system register.
 * This will trap in sys_regs.c and call this function.
 * This ICC_SGI1R_EL1 register contains the upper three affinity levels of the
 * target processors as well as a bitmask of 16 Aff0 CPUs.
 *
 * If the interrupt routing mode bit is not set, we iterate over the Aff0
 * bits and signal the VCPUs matching the provided Aff{3,2,1}.
 *
 * If this bit is set, we signal all, but not the calling VCPU.
 */
void vgic_v3_dispatch_sgi(struct kvm_vcpu *vcpu, u64 reg, bool allow_group1)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_vcpu *c_vcpu;
	unsigned long target_cpus;
	u64 mpidr;
	u32 sgi, aff0;
	unsigned long c;

	sgi = FIELD_GET(ICC_SGI1R_SGI_ID_MASK, reg);

	/* Broadcast */
	if (unlikely(reg & BIT_ULL(ICC_SGI1R_IRQ_ROUTING_MODE_BIT))) {
		kvm_for_each_vcpu(c, c_vcpu, kvm) {
			/* Don't signal the calling VCPU */
			if (c_vcpu == vcpu)
				continue;

			vgic_v3_queue_sgi(c_vcpu, sgi, allow_group1);
		}

		return;
	}

	/* We iterate over affinities to find the corresponding vcpus */
	mpidr = SGI_AFFINITY_LEVEL(reg, 3);
	mpidr |= SGI_AFFINITY_LEVEL(reg, 2);
	mpidr |= SGI_AFFINITY_LEVEL(reg, 1);
	target_cpus = FIELD_GET(ICC_SGI1R_TARGET_LIST_MASK, reg);

	for_each_set_bit(aff0, &target_cpus, hweight_long(ICC_SGI1R_TARGET_LIST_MASK)) {
		c_vcpu = kvm_mpidr_to_vcpu(kvm, mpidr | aff0);
		if (c_vcpu)
			vgic_v3_queue_sgi(c_vcpu, sgi, allow_group1);
	}
}

int vgic_v3_dist_uaccess(struct kvm_vcpu *vcpu, bool is_write,
			 int offset, u32 *val)
{
	struct vgic_io_device dev = {
		.regions = vgic_v3_dist_registers,
		.nr_regions = ARRAY_SIZE(vgic_v3_dist_registers),
	};

	return vgic_uaccess(vcpu, &dev, is_write, offset, val);
}

int vgic_v3_redist_uaccess(struct kvm_vcpu *vcpu, bool is_write,
			   int offset, u32 *val)
{
	struct vgic_io_device rd_dev = {
		.regions = vgic_v3_rd_registers,
		.nr_regions = ARRAY_SIZE(vgic_v3_rd_registers),
	};

	return vgic_uaccess(vcpu, &rd_dev, is_write, offset, val);
}

int vgic_v3_line_level_info_uaccess(struct kvm_vcpu *vcpu, bool is_write,
				    u32 intid, u32 *val)
{
	if (intid % 32)
		return -EINVAL;

	if (is_write)
		vgic_write_irq_line_level_info(vcpu, intid, *val);
	else
		*val = vgic_read_irq_line_level_info(vcpu, intid);

	return 0;
}
