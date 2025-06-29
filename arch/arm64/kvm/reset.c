// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * Derived from arch/arm/kvm/reset.c
 * Copyright (C) 2012 - Virtual Open Systems and Columbia University
 * Author: Christoffer Dall <c.dall@virtualopensystems.com>
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/kvm.h>
#include <linux/hw_breakpoint.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#include <kvm/arm_arch_timer.h>

#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/fpsimd.h>
#include <asm/ptrace.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/virt.h>

/* Maximum phys_shift supported for any VM on this host */
static u32 kvm_ipa_limit;

unsigned int kvm_sve_max_vl;
unsigned int kvm_host_sve_max_vl;

int kvm_arm_init_sve(void)
{
	if (system_supports_sve()) {
		kvm_sve_max_vl = sve_max_virtualisable_vl();
		kvm_host_sve_max_vl = sve_max_vl();
		kvm_nvhe_sym(kvm_host_sve_max_vl) = kvm_host_sve_max_vl;

		/*
		 * The get_sve_reg()/set_sve_reg() ioctl interface will need
		 * to be extended with multiple register slice support in
		 * order to support vector lengths greater than
		 * VL_ARCH_MAX:
		 */
		if (WARN_ON(kvm_sve_max_vl > VL_ARCH_MAX))
			kvm_sve_max_vl = VL_ARCH_MAX;

		/*
		 * Don't even try to make use of vector lengths that
		 * aren't available on all CPUs, for now:
		 */
		if (kvm_sve_max_vl < sve_max_vl())
			pr_warn("KVM: SVE vector length for guests limited to %u bytes\n",
				kvm_sve_max_vl);
	}

	return 0;
}

static int kvm_vcpu_enable_sve(struct kvm_vcpu *vcpu)
{
	if (!system_supports_sve())
		return -EINVAL;

	vcpu->arch.sve_max_vl = kvm_sve_max_vl;

	/*
	 * Userspace can still customize the vector lengths by writing
	 * KVM_REG_ARM64_SVE_VLS.  Allocation is deferred until
	 * kvm_arm_vcpu_finalize(), which freezes the configuration.
	 */
	vcpu_set_flag(vcpu, GUEST_HAS_SVE);

	return 0;
}

/*
 * Finalize vcpu's maximum SVE vector length, allocating
 * vcpu->arch.sve_state as necessary.
 */
static int kvm_vcpu_finalize_sve(struct kvm_vcpu *vcpu)
{
	void *buf;
	unsigned int vl;
	size_t reg_sz;
	int ret;

	vl = vcpu->arch.sve_max_vl;

	/*
	 * Responsibility for these properties is shared between
	 * kvm_arm_init_sve(), kvm_vcpu_enable_sve() and
	 * set_sve_vls().  Double-check here just to be sure:
	 */
	if (WARN_ON(!sve_vl_valid(vl) || vl > sve_max_virtualisable_vl() ||
		    vl > VL_ARCH_MAX))
		return -EIO;

	reg_sz = vcpu_sve_state_size(vcpu);
	buf = kzalloc(reg_sz, GFP_KERNEL_ACCOUNT);
	if (!buf)
		return -ENOMEM;

	ret = kvm_share_hyp(buf, buf + reg_sz);
	if (ret) {
		kfree(buf);
		return ret;
	}

	vcpu->arch.sve_state = buf;
	vcpu_set_flag(vcpu, VCPU_SVE_FINALIZED);
	return 0;
}

int kvm_arm_vcpu_finalize(struct kvm_vcpu *vcpu, int feature)
{
	switch (feature) {
	case KVM_ARM_VCPU_SVE:
		if (!vcpu_has_sve(vcpu))
			return -EINVAL;

		if (kvm_arm_vcpu_sve_finalized(vcpu))
			return -EPERM;

		return kvm_vcpu_finalize_sve(vcpu);
	}

	return -EINVAL;
}

bool kvm_arm_vcpu_is_finalized(struct kvm_vcpu *vcpu)
{
	if (vcpu_has_sve(vcpu) && !kvm_arm_vcpu_sve_finalized(vcpu))
		return false;

	return true;
}

void kvm_arm_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	void *sve_state = vcpu->arch.sve_state;

	kvm_unshare_hyp(vcpu, vcpu + 1);
	if (sve_state)
		kvm_unshare_hyp(sve_state, sve_state + vcpu_sve_state_size(vcpu));
	kfree(sve_state);
}

static void kvm_vcpu_reset_sve(struct kvm_vcpu *vcpu)
{
	if (vcpu_has_sve(vcpu))
		memset(vcpu->arch.sve_state, 0, vcpu_sve_state_size(vcpu));
}

/**
 * kvm_set_vm_width() - set the register width for the guest
 * @vcpu: Pointer to the vcpu being configured
 *
 * Set both KVM_ARCH_FLAG_EL1_32BIT and KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED
 * in the VM flags based on the vcpu's requested register width, the HW
 * capabilities and other options (such as MTE).
 * When REG_WIDTH_CONFIGURED is already set, the vcpu settings must be
 * consistent with the value of the FLAG_EL1_32BIT bit in the flags.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int kvm_set_vm_width(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	bool is32bit;

	is32bit = vcpu_has_feature(vcpu, KVM_ARM_VCPU_EL1_32BIT);

	lockdep_assert_held(&kvm->arch.config_lock);

	if (test_bit(KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED, &kvm->arch.flags)) {
		/*
		 * The guest's register width is already configured.
		 * Make sure that the vcpu is consistent with it.
		 */
		if (is32bit == test_bit(KVM_ARCH_FLAG_EL1_32BIT, &kvm->arch.flags))
			return 0;

		return -EINVAL;
	}

	if (!cpus_have_const_cap(ARM64_HAS_32BIT_EL1) && is32bit)
		return -EINVAL;

	/* MTE is incompatible with AArch32 */
	if (kvm_has_mte(kvm) && is32bit)
		return -EINVAL;

	if (is32bit)
		set_bit(KVM_ARCH_FLAG_EL1_32BIT, &kvm->arch.flags);

	set_bit(KVM_ARCH_FLAG_REG_WIDTH_CONFIGURED, &kvm->arch.flags);

	return 0;
}

/**
 * kvm_reset_vcpu - sets core registers and sys_regs to reset value
 * @vcpu: The VCPU pointer
 *
 * This function sets the registers on the virtual CPU struct to their
 * architecturally defined reset values, except for registers whose reset is
 * deferred until kvm_arm_vcpu_finalize().
 *
 * Note: This function can be called from two paths: The KVM_ARM_VCPU_INIT
 * ioctl or as part of handling a request issued by another VCPU in the PSCI
 * handling code.  In the first case, the VCPU will not be loaded, and in the
 * second case the VCPU will be loaded.  Because this function operates purely
 * on the memory-backed values of system registers, we want to do a full put if
 * we were loaded (handling a request) and load the values back at the end of
 * the function.  Otherwise we leave the state alone.  In both cases, we
 * disable preemption around the vcpu reset as we would otherwise race with
 * preempt notifiers which also call put/load.
 */
int kvm_reset_vcpu(struct kvm_vcpu *vcpu)
{
	struct vcpu_reset_state reset_state;
	int ret;
	bool loaded;

	mutex_lock(&vcpu->kvm->arch.config_lock);
	ret = kvm_set_vm_width(vcpu);
	mutex_unlock(&vcpu->kvm->arch.config_lock);

	if (ret)
		return ret;

	spin_lock(&vcpu->arch.mp_state_lock);
	reset_state = vcpu->arch.reset_state;
	vcpu->arch.reset_state.reset = false;
	spin_unlock(&vcpu->arch.mp_state_lock);

	/* Reset PMU outside of the non-preemptible section */
	kvm_pmu_vcpu_reset(vcpu);

	preempt_disable();
	loaded = (vcpu->cpu != -1);
	if (loaded)
		kvm_arch_vcpu_put(vcpu);

	if (!kvm_arm_vcpu_sve_finalized(vcpu)) {
		if (test_bit(KVM_ARM_VCPU_SVE, vcpu->arch.features)) {
			ret = kvm_vcpu_enable_sve(vcpu);
			if (ret)
				goto out;
		}
	} else {
		kvm_vcpu_reset_sve(vcpu);
	}

	if (test_bit(KVM_ARM_VCPU_PTRAUTH_ADDRESS, vcpu->arch.features) ||
	    test_bit(KVM_ARM_VCPU_PTRAUTH_GENERIC, vcpu->arch.features)) {
		if (kvm_vcpu_enable_ptrauth(vcpu)) {
			ret = -EINVAL;
			goto out;
		}
	}

	if (kvm_vcpu_has_pmu(vcpu) && !kvm_arm_support_pmu_v3()) {
		ret = -EINVAL;
		goto out;
	}

	/* Reset core registers */
	kvm_reset_vcpu_core(vcpu);

	/* Reset system registers */
	kvm_reset_sys_regs(vcpu);

	/*
	 * Additional reset state handling that PSCI may have imposed on us.
	 * Must be done after all the sys_reg reset.
	 */
	if (reset_state.reset)
		kvm_reset_vcpu_psci(vcpu, &reset_state);

	/* Reset timer */
	ret = kvm_timer_vcpu_reset(vcpu);
out:
	if (loaded)
		kvm_arch_vcpu_load(vcpu, smp_processor_id());
	preempt_enable();
	return ret;
}

u32 get_kvm_ipa_limit(void)
{
	return kvm_ipa_limit;
}

int kvm_set_ipa_limit(void)
{
	unsigned int parange;
	u64 mmfr0;

	mmfr0 = read_sanitised_ftr_reg(SYS_ID_AA64MMFR0_EL1);
	parange = cpuid_feature_extract_unsigned_field(mmfr0,
				ID_AA64MMFR0_EL1_PARANGE_SHIFT);
	/*
	 * IPA size beyond 48 bits could not be supported
	 * on either 4K or 16K page size. Hence let's cap
	 * it to 48 bits, in case it's reported as larger
	 * on the system.
	 */
	if (PAGE_SIZE != SZ_64K)
		parange = min(parange, (unsigned int)ID_AA64MMFR0_EL1_PARANGE_48);

	/*
	 * Check with ARMv8.5-GTG that our PAGE_SIZE is supported at
	 * Stage-2. If not, things will stop very quickly.
	 */
	switch (cpuid_feature_extract_unsigned_field(mmfr0, ID_AA64MMFR0_EL1_TGRAN_2_SHIFT)) {
	case ID_AA64MMFR0_EL1_TGRAN_2_SUPPORTED_NONE:
		kvm_err("PAGE_SIZE not supported at Stage-2, giving up\n");
		return -EINVAL;
	case ID_AA64MMFR0_EL1_TGRAN_2_SUPPORTED_DEFAULT:
		kvm_debug("PAGE_SIZE supported at Stage-2 (default)\n");
		break;
	case ID_AA64MMFR0_EL1_TGRAN_2_SUPPORTED_MIN ... ID_AA64MMFR0_EL1_TGRAN_2_SUPPORTED_MAX:
		kvm_debug("PAGE_SIZE supported at Stage-2 (advertised)\n");
		break;
	default:
		kvm_err("Unsupported value for TGRAN_2, giving up\n");
		return -EINVAL;
	}

	kvm_ipa_limit = id_aa64mmfr0_parange_to_phys_shift(parange);
	kvm_info("IPA Size Limit: %d bits%s\n", kvm_ipa_limit,
		 ((kvm_ipa_limit < KVM_PHYS_SHIFT) ?
		  " (Reduced IPA size, limited VM/VMM compatibility)" : ""));

	return 0;
}
