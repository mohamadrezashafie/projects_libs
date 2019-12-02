/*
 * Copyright (c) 2016, NVIDIA CORPORATION.
 * Copright 2019, Data61
 * Commonwealth Scientific and Industrial Research Organisation (CSIRO)
 * ABN 41 687 119 230.
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(DATA61_GPL)
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>

#include <platsupport/pmem.h>
#include <tx2bpmp/hsp.h>
#include <utils/util.h>

/* Register holds information about the number of
 * shared mailboxes, shared semaphores etc. */
#define HSP_INT_DIMENSION_OFFSET 0x380
#define HSP_INT_DIMENSION_SM_SHIFT 0
#define HSP_INT_DIMENSION_SS_SHIFT 4
#define HSP_INT_DIMENSION_AS_SHIFT 8
#define HSP_INT_DIMENSION_NUM_MASK 0xf

#define HSP_DOORBELL_BLOCK_STRIDE 0x100

#define HSP_BITMAP_TZ_SECURE_SHFIT 0
#define HSP_BITMAP_TZ_NONSECURE_SHIFT 16

enum dbell_reg_offset {
    DBELL_TRIGGER = 0x0,
    DBELL_ENABLE = 0x4,
    DBELL_RAW = 0x8,
    DBELL_PENDING = 0xc
};

enum dbell_bitmap_offset {
    CCPLEX_BIT = BIT(1),
    DPMU_BIT = BIT(2),
    BPMP_BIT = BIT(3),
    SPE_BIT = BIT(4),
    CPE_BIT = BIT(5),
    SCE_BIT = CPE_BIT,
    DMA_BIT = BIT(6),
    TSECA_BIT = BIT(7),
    TSECB_BIT = BIT(8),
    JTAGM_BIT = BIT(9),
    CSITE_BIT = BIT(10),
    APE_BIT = BIT(11)
};

static pmem_region_t tx2_hsp_region = {
    .type = PMEM_TYPE_DEVICE,
    .base_addr = TX2_HSP_PADDR,
    .length = TX2_HSP_SIZE
};

static bool check_doorbell_id_is_valid(enum tx2_doorbell_id db_id)
{
    if (CCPLEX_PM_DBELL <= db_id && db_id <= APE_DBELL) {
        return true;
    }
    return false;
}

static uint32_t *tx2_hsp_get_doorbell_register(tx2_hsp_t *hsp, enum tx2_doorbell_id db_id,
                                               enum dbell_reg_offset offset)
{
    assert(hsp);
    assert(DBELL_TRIGGER <= offset && offset <= DBELL_PENDING);
    return hsp->doorbell_base + db_id * HSP_DOORBELL_BLOCK_STRIDE + offset;
}

int tx2_hsp_init(ps_io_ops_t *io_ops, tx2_hsp_t *hsp)
{
    if (!io_ops || !hsp) {
        ZF_LOGE("Arguments are NULL!");
        return -EINVAL;
    }

    hsp->hsp_base = ps_pmem_map(io_ops, tx2_hsp_region, false, PS_MEM_NORMAL);
    if (!hsp->hsp_base) {
        ZF_LOGE("Failed to map tx2 HSP module");
        return -ENOMEM;
    }

    /* Get the base addr of the doorbell
     * Section 14.8.5: All doorbell registers are in a single page, doorbell
     * {db} has a register range starting at DB{db}_BASE = HSP_{inst}_BASE +
     * (1+ nSM/2 + nSS + nAS) * 64 KiB + {db} * 0x100. */

    int num_sm = 0, num_ss = 0, num_as = 0;

    uint32_t *int_dim_reg = hsp->hsp_base + HSP_INT_DIMENSION_OFFSET;

    num_sm = (*int_dim_reg >> HSP_INT_DIMENSION_SM_SHIFT) & HSP_INT_DIMENSION_NUM_MASK;
    num_ss = (*int_dim_reg >> HSP_INT_DIMENSION_SS_SHIFT) & HSP_INT_DIMENSION_NUM_MASK;
    num_as = (*int_dim_reg >> HSP_INT_DIMENSION_AS_SHIFT) & HSP_INT_DIMENSION_NUM_MASK;

    hsp->doorbell_base = hsp->hsp_base + (1 + (num_sm / 2) + num_ss + num_as) * 0x10000;

    return 0;
}

int tx2_hsp_destroy(ps_io_ops_t *io_ops, tx2_hsp_t *hsp)
{
    if (!io_ops || !hsp) {
        ZF_LOGE("Arguments are NULL!");
        return -EINVAL;
    }

    if (hsp->hsp_base) {
        ps_io_unmap(&io_ops->io_mapper, hsp->hsp_base, tx2_hsp_region.length);
    }

    return 0;
}

int tx2_hsp_doorbell_ring(tx2_hsp_t *hsp, enum tx2_doorbell_id db_id)
{
    if (!hsp) {
        ZF_LOGE("Arguments are NULL!");
        return -EINVAL;
    }

    if (!check_doorbell_id_is_valid(db_id)) {
        ZF_LOGE("Invalid doorbell ID!");
        return -EINVAL;
    }


    /* Write any value to the trigger register to 'ring' the doorbell */
    uint32_t *trigger_reg = tx2_hsp_get_doorbell_register(hsp, db_id, DBELL_TRIGGER);
    assert(trigger_reg);
    *trigger_reg = 1;

    return 0;
}

int tx2_hsp_doorbell_check(tx2_hsp_t *hsp, enum tx2_doorbell_id db_id)
{
    if (!hsp) {
        ZF_LOGE("Arguments are NULL!");
        return -EINVAL;
    }

    if (!check_doorbell_id_is_valid(db_id)) {
        ZF_LOGE("Invalid doorbell ID!");
        return -EINVAL;
    }

    /* Checking if the doorbell has been 'rung' requires checking for proper
     * bit in the bitfield. The bitfield is also split into TrustZone secure
     * and TZ non-secure. Refer to Figure 75 in Section 14.8.5 for further details. */
    uint32_t *pending_reg = tx2_hsp_get_doorbell_register(hsp, db_id, DBELL_PENDING);

    enum dbell_bitmap_offset bitmap_offset;
    switch (db_id) {
    case CCPLEX_PM_DBELL:
    case CCPLEX_TZ_UNSECURE_DBELL:
    case CCPLEX_TZ_SECURE_DBELL:
        bitmap_offset = CCPLEX_BIT;
        break;
    case BPMP_DBELL:
        bitmap_offset = BPMP_BIT;
        break;
    case SPE_DBELL:
        bitmap_offset = SPE_BIT;
        break;
    case SCE_DBELL:
        bitmap_offset = SCE_BIT;
        break;
    case APE_DBELL:
        bitmap_offset = APE_BIT;
        break;
    default:
        ZF_LOGF("We shouldn't get here, doorbell ID is %d", db_id);
    }

    /* Usermode isn't in TrustZone secure, so we just default to TZ non-secure */
    int is_pending = *pending_reg & (bitmap_offset << HSP_BITMAP_TZ_NONSECURE_SHIFT);

    if (is_pending) {
        *pending_reg &= ~(bitmap_offset << HSP_BITMAP_TZ_NONSECURE_SHIFT);
    }

    return (is_pending != 0);
}