/*
 * vpmu.c: PMU virtualization for HVM domain.
 *
 * Copyright (c) 2010, Advanced Micro Devices, Inc.
 * Parts of this code are Copyright (c) 2007, Intel Corporation
 *
 * Author: Wei Wang <wei.wang2@amd.com>
 * Tested by: Suravee Suthikulpanit <Suravee.Suthikulpanit@amd.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 */

#include <xen/config.h>
#include <xen/xenoprof.h>
#include <xen/hvm/save.h>
#include <xen/sched.h>
#include <xen/irq.h>
#include <asm/apic.h>
#include <asm/hvm/vlapic.h>
#include <asm/hvm/vpmu.h>
#include <public/pmu.h>

#define MSR_F10H_EVNTSEL_GO_SHIFT   40
#define MSR_F10H_EVNTSEL_EN_SHIFT   22
#define MSR_F10H_COUNTER_LENGTH     48

#define is_guest_mode(msr) ((msr) & (1ULL << MSR_F10H_EVNTSEL_GO_SHIFT))
#define is_pmu_enabled(msr) ((msr) & (1ULL << MSR_F10H_EVNTSEL_EN_SHIFT))
#define set_guest_mode(msr) (msr |= (1ULL << MSR_F10H_EVNTSEL_GO_SHIFT))
#define is_overflowed(msr) (!((msr) & (1ULL << (MSR_F10H_COUNTER_LENGTH-1))))

static unsigned int __read_mostly num_counters;
static const u32 __read_mostly *counters;
static const u32 __read_mostly *ctrls;
static bool_t __read_mostly k7_counters_mirrored;

#define F10H_NUM_COUNTERS   4
#define F15H_NUM_COUNTERS   6

/* PMU Counter MSRs. */
static const u32 AMD_F10H_COUNTERS[] = {
    MSR_K7_PERFCTR0,
    MSR_K7_PERFCTR1,
    MSR_K7_PERFCTR2,
    MSR_K7_PERFCTR3
};

/* PMU Control MSRs. */
static const u32 AMD_F10H_CTRLS[] = {
    MSR_K7_EVNTSEL0,
    MSR_K7_EVNTSEL1,
    MSR_K7_EVNTSEL2,
    MSR_K7_EVNTSEL3
};

static const u32 AMD_F15H_COUNTERS[] = {
    MSR_AMD_FAM15H_PERFCTR0,
    MSR_AMD_FAM15H_PERFCTR1,
    MSR_AMD_FAM15H_PERFCTR2,
    MSR_AMD_FAM15H_PERFCTR3,
    MSR_AMD_FAM15H_PERFCTR4,
    MSR_AMD_FAM15H_PERFCTR5
};

static const u32 AMD_F15H_CTRLS[] = {
    MSR_AMD_FAM15H_EVNTSEL0,
    MSR_AMD_FAM15H_EVNTSEL1,
    MSR_AMD_FAM15H_EVNTSEL2,
    MSR_AMD_FAM15H_EVNTSEL3,
    MSR_AMD_FAM15H_EVNTSEL4,
    MSR_AMD_FAM15H_EVNTSEL5
};

/* Use private context as a flag for MSR bitmap */
#define msr_bitmap_on(vpmu)    do {                                    \
                                   (vpmu)->priv_context = (void *)-1L; \
                               } while (0)
#define msr_bitmap_off(vpmu)   do {                                    \
                                   (vpmu)->priv_context = NULL;        \
                               } while (0)
#define is_msr_bitmap_on(vpmu) ((vpmu)->priv_context != NULL)

static inline int get_pmu_reg_type(u32 addr)
{
    if ( (addr >= MSR_K7_EVNTSEL0) && (addr <= MSR_K7_EVNTSEL3) )
        return MSR_TYPE_CTRL;

    if ( (addr >= MSR_K7_PERFCTR0) && (addr <= MSR_K7_PERFCTR3) )
        return MSR_TYPE_COUNTER;

    if ( (addr >= MSR_AMD_FAM15H_EVNTSEL0) &&
         (addr <= MSR_AMD_FAM15H_PERFCTR5 ) )
    {
        if (addr & 1)
            return MSR_TYPE_COUNTER;
        else
            return MSR_TYPE_CTRL;
    }

    /* unsupported registers */
    return -1;
}

static inline u32 get_fam15h_addr(u32 addr)
{
    switch ( addr )
    {
    case MSR_K7_PERFCTR0:
        return MSR_AMD_FAM15H_PERFCTR0;
    case MSR_K7_PERFCTR1:
        return MSR_AMD_FAM15H_PERFCTR1;
    case MSR_K7_PERFCTR2:
        return MSR_AMD_FAM15H_PERFCTR2;
    case MSR_K7_PERFCTR3:
        return MSR_AMD_FAM15H_PERFCTR3;
    case MSR_K7_EVNTSEL0:
        return MSR_AMD_FAM15H_EVNTSEL0;
    case MSR_K7_EVNTSEL1:
        return MSR_AMD_FAM15H_EVNTSEL1;
    case MSR_K7_EVNTSEL2:
        return MSR_AMD_FAM15H_EVNTSEL2;
    case MSR_K7_EVNTSEL3:
        return MSR_AMD_FAM15H_EVNTSEL3;
    default:
        break;
    }

    return addr;
}

static void amd_vpmu_set_msr_bitmap(struct vcpu *v)
{
    unsigned int i;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    for ( i = 0; i < num_counters; i++ )
    {
        svm_intercept_msr(v, counters[i], MSR_INTERCEPT_NONE);
        svm_intercept_msr(v, ctrls[i], MSR_INTERCEPT_WRITE);
    }

    msr_bitmap_on(vpmu);
}

static void amd_vpmu_unset_msr_bitmap(struct vcpu *v)
{
    unsigned int i;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    for ( i = 0; i < num_counters; i++ )
    {
        svm_intercept_msr(v, counters[i], MSR_INTERCEPT_RW);
        svm_intercept_msr(v, ctrls[i], MSR_INTERCEPT_RW);
    }

    msr_bitmap_off(vpmu);
}

static int amd_vpmu_do_interrupt(struct cpu_user_regs *regs)
{
    return 1;
}

static inline void context_load(struct vcpu *v)
{
    unsigned int i;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);
    struct xen_pmu_amd_ctxt *ctxt = vpmu->context;
    uint64_t *counter_regs = vpmu_reg_pointer(ctxt, counters);
    uint64_t *ctrl_regs = vpmu_reg_pointer(ctxt, ctrls);

    for ( i = 0; i < num_counters; i++ )
    {
        wrmsrl(counters[i], counter_regs[i]);
        wrmsrl(ctrls[i], ctrl_regs[i]);
    }
}

static void amd_vpmu_load(struct vcpu *v)
{
    struct vpmu_struct *vpmu = vcpu_vpmu(v);
    struct xen_pmu_amd_ctxt *ctxt = vpmu->context;
    uint64_t *ctrl_regs = vpmu_reg_pointer(ctxt, ctrls);

    vpmu_reset(vpmu, VPMU_FROZEN);

    if ( vpmu_is_set(vpmu, VPMU_CONTEXT_LOADED) )
    {
        unsigned int i;

        for ( i = 0; i < num_counters; i++ )
            wrmsrl(ctrls[i], ctrl_regs[i]);

        return;
    }

    vpmu_set(vpmu, VPMU_CONTEXT_LOADED);

    context_load(v);
}

static inline void context_save(struct vcpu *v)
{
    unsigned int i;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);
    struct xen_pmu_amd_ctxt *ctxt = vpmu->context;
    uint64_t *counter_regs = vpmu_reg_pointer(ctxt, counters);

    /* No need to save controls -- they are saved in amd_vpmu_do_wrmsr */
    for ( i = 0; i < num_counters; i++ )
        rdmsrl(counters[i], counter_regs[i]);
}

static int amd_vpmu_save(struct vcpu *v)
{
    struct vpmu_struct *vpmu = vcpu_vpmu(v);
    unsigned int i;

    /*
     * Stop the counters. If we came here via vpmu_save_force (i.e.
     * when VPMU_CONTEXT_SAVE is set) counters are already stopped.
     */
    if ( !vpmu_is_set(vpmu, VPMU_CONTEXT_SAVE) )
    {
        vpmu_set(vpmu, VPMU_FROZEN);

        for ( i = 0; i < num_counters; i++ )
            wrmsrl(ctrls[i], 0);

        return 0;
    }

    if ( !vpmu_is_set(vpmu, VPMU_CONTEXT_LOADED) )
        return 0;

    context_save(v);

    if ( !vpmu_is_set(vpmu, VPMU_RUNNING) &&
         has_hvm_container_vcpu(v) && is_msr_bitmap_on(vpmu) )
        amd_vpmu_unset_msr_bitmap(v);

    return 1;
}

static void context_update(unsigned int msr, u64 msr_content)
{
    unsigned int i;
    struct vcpu *v = current;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);
    struct xen_pmu_amd_ctxt *ctxt = vpmu->context;
    uint64_t *counter_regs = vpmu_reg_pointer(ctxt, counters);
    uint64_t *ctrl_regs = vpmu_reg_pointer(ctxt, ctrls);

    if ( k7_counters_mirrored &&
        ((msr >= MSR_K7_EVNTSEL0) && (msr <= MSR_K7_PERFCTR3)) )
    {
        msr = get_fam15h_addr(msr);
    }

    for ( i = 0; i < num_counters; i++ )
    {
       if ( msr == ctrls[i] )
       {
           ctrl_regs[i] = msr_content;
           return;
       }
        else if (msr == counters[i] )
        {
            counter_regs[i] = msr_content;
            return;
        }
    }
}

static int amd_vpmu_do_wrmsr(unsigned int msr, uint64_t msr_content,
                             uint64_t supported)
{
    struct vcpu *v = current;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    ASSERT(!supported);

    /* For all counters, enable guest only mode for HVM guest */
    if ( has_hvm_container_vcpu(v) &&
         (get_pmu_reg_type(msr) == MSR_TYPE_CTRL) &&
         !is_guest_mode(msr_content) )
    {
        set_guest_mode(msr_content);
    }

    /* check if the first counter is enabled */
    if ( (get_pmu_reg_type(msr) == MSR_TYPE_CTRL) &&
        is_pmu_enabled(msr_content) && !vpmu_is_set(vpmu, VPMU_RUNNING) )
    {
        if ( !acquire_pmu_ownership(PMU_OWNER_HVM) )
            return 1;
        vpmu_set(vpmu, VPMU_RUNNING);

        if ( has_hvm_container_vcpu(v) && is_msr_bitmap_on(vpmu) )
             amd_vpmu_set_msr_bitmap(v);
    }

    /* stop saving & restore if guest stops first counter */
    if ( (get_pmu_reg_type(msr) == MSR_TYPE_CTRL) &&
        (is_pmu_enabled(msr_content) == 0) && vpmu_is_set(vpmu, VPMU_RUNNING) )
    {
        vpmu_reset(vpmu, VPMU_RUNNING);
        if ( has_hvm_container_vcpu(v) && is_msr_bitmap_on(vpmu) )
             amd_vpmu_unset_msr_bitmap(v);
        release_pmu_ownship(PMU_OWNER_HVM);
    }

    if ( !vpmu_is_set(vpmu, VPMU_CONTEXT_LOADED)
        || vpmu_is_set(vpmu, VPMU_FROZEN) )
    {
        context_load(v);
        vpmu_set(vpmu, VPMU_CONTEXT_LOADED);
        vpmu_reset(vpmu, VPMU_FROZEN);
    }

    /* Update vpmu context immediately */
    context_update(msr, msr_content);

    /* Write to hw counters */
    wrmsrl(msr, msr_content);
    return 1;
}

static int amd_vpmu_do_rdmsr(unsigned int msr, uint64_t *msr_content)
{
    struct vcpu *v = current;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    if ( !vpmu_is_set(vpmu, VPMU_CONTEXT_LOADED)
        || vpmu_is_set(vpmu, VPMU_FROZEN) )
    {
        context_load(v);
        vpmu_set(vpmu, VPMU_CONTEXT_LOADED);
        vpmu_reset(vpmu, VPMU_FROZEN);
    }

    rdmsrl(msr, *msr_content);

    return 1;
}

static void amd_vpmu_destroy(struct vcpu *v)
{
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    if ( has_hvm_container_vcpu(v) && is_msr_bitmap_on(vpmu) )
        amd_vpmu_unset_msr_bitmap(v);

    xfree(vpmu->context);

    if ( vpmu_is_set(vpmu, VPMU_RUNNING) )
        release_pmu_ownship(PMU_OWNER_HVM);

    vpmu_clear(vpmu);
}

/* VPMU part of the 'q' keyhandler */
static void amd_vpmu_dump(const struct vcpu *v)
{
    const struct vpmu_struct *vpmu = vcpu_vpmu(v);
    const struct xen_pmu_amd_ctxt *ctxt = vpmu->context;
    const uint64_t *counter_regs = vpmu_reg_pointer(ctxt, counters);
    const uint64_t *ctrl_regs = vpmu_reg_pointer(ctxt, ctrls);
    unsigned int i;

    printk("    VPMU state: 0x%x ", vpmu->flags);
    if ( !vpmu_is_set(vpmu, VPMU_CONTEXT_ALLOCATED) )
    {
         printk("\n");
         return;
    }

    printk("(");
    if ( vpmu_is_set(vpmu, VPMU_PASSIVE_DOMAIN_ALLOCATED) )
        printk("PASSIVE_DOMAIN_ALLOCATED, ");
    if ( vpmu_is_set(vpmu, VPMU_FROZEN) )
        printk("FROZEN, ");
    if ( vpmu_is_set(vpmu, VPMU_CONTEXT_SAVE) )
        printk("SAVE, ");
    if ( vpmu_is_set(vpmu, VPMU_RUNNING) )
        printk("RUNNING, ");
    if ( vpmu_is_set(vpmu, VPMU_CONTEXT_LOADED) )
        printk("LOADED, ");
    printk("ALLOCATED)\n");

    for ( i = 0; i < num_counters; i++ )
    {
        uint64_t ctrl, cntr;

        rdmsrl(ctrls[i], ctrl);
        rdmsrl(counters[i], cntr);
        printk("      %#x: %#lx (%#lx in HW)    %#x: %#lx (%#lx in HW)\n",
               ctrls[i], ctrl_regs[i], ctrl,
               counters[i], counter_regs[i], cntr);
    }
}

struct arch_vpmu_ops amd_vpmu_ops = {
    .do_wrmsr = amd_vpmu_do_wrmsr,
    .do_rdmsr = amd_vpmu_do_rdmsr,
    .do_interrupt = amd_vpmu_do_interrupt,
    .arch_vpmu_destroy = amd_vpmu_destroy,
    .arch_vpmu_save = amd_vpmu_save,
    .arch_vpmu_load = amd_vpmu_load,
    .arch_vpmu_dump = amd_vpmu_dump
};

int svm_vpmu_initialise(struct vcpu *v)
{
    struct xen_pmu_amd_ctxt *ctxt;
    struct vpmu_struct *vpmu = vcpu_vpmu(v);

    if ( vpmu_mode == XENPMU_MODE_OFF )
        return 0;

    if ( !counters )
        return -EINVAL;

    ctxt = xzalloc_bytes(sizeof(*ctxt) +
                         2 * sizeof(uint64_t) * num_counters);
    if ( !ctxt )
    {
        printk(XENLOG_G_WARNING "Insufficient memory for PMU, "
               " PMU feature is unavailable on domain %d vcpu %d.\n",
               v->vcpu_id, v->domain->domain_id);
        return -ENOMEM;
    }

    ctxt->counters = sizeof(*ctxt);
    ctxt->ctrls = ctxt->counters + sizeof(uint64_t) * num_counters;

    vpmu->context = ctxt;
    vpmu->priv_context = NULL;

    vpmu->arch_vpmu_ops = &amd_vpmu_ops;

    vpmu_set(vpmu, VPMU_CONTEXT_ALLOCATED);
    return 0;
}

int __init amd_vpmu_init(void)
{
    switch ( current_cpu_data.x86 )
    {
    case 0x15:
        num_counters = F15H_NUM_COUNTERS;
        counters = AMD_F15H_COUNTERS;
        ctrls = AMD_F15H_CTRLS;
        k7_counters_mirrored = 1;
        break;
    case 0x10:
    case 0x12:
    case 0x14:
    case 0x16:
        num_counters = F10H_NUM_COUNTERS;
        counters = AMD_F10H_COUNTERS;
        ctrls = AMD_F10H_CTRLS;
        k7_counters_mirrored = 0;
        break;
    default:
        printk(XENLOG_WARNING "VPMU: Unsupported CPU family %#x\n",
               current_cpu_data.x86);
        return -EINVAL;
    }

    if ( sizeof(struct xen_pmu_data) +
         2 * sizeof(uint64_t) * num_counters > PAGE_SIZE )
    {
        printk(XENLOG_WARNING
               "VPMU: Register bank does not fit into VPMU shared page\n");
        counters = ctrls = NULL;
        num_counters = 0;
        return -ENOSPC;
    }

    return 0;
}

