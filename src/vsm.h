/**
 * @file vsm.h
 * @brief The VSMLIST data area, as z/OS documents it.
 *
 * The layout comes from z/OS MVS Programming: Authorized Assembler Services
 * Guide, "Virtual Storage Management", figures 37-46.  The work-area header
 * (return code, data-area address, data-area length) is not part of this
 * file: src/ztop.c owns the header and hands the data area here.
 */
#ifndef ZTOP_VSM_H
#define ZTOP_VSM_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Area codes, the same numbers the assembler stub takes. */
enum vsm_area { VSM_CSA = 1, VSM_SQA = 2, VSM_LSQA = 3, VSM_PVT = 4 };

/** @brief SPACE= requests. VSM_FREE includes the allocated information. */
enum vsm_space { VSM_FREE = 1, VSM_UNALLOC = 2 };

/** @brief Rows kept per area; PVT can hold several TCBs of subpools. */
#define VSM_MAX_SUB 512

/** @brief Region descriptors kept for display (CSA/ECSA/RUCSA, PVT regions). */
#define VSM_MAX_REGION 16

/** @brief One subpool: allocated blocks, the free space in them, the real use.
 */
struct vsm_subpool {
    unsigned id;      /**< translated subpool number */
    unsigned key;     /**< storage key from the descriptor's flag byte */
    unsigned flags;   /**< the descriptor's flag byte */
    uint32_t tcb;     /**< owning TCB for PVT, zero for common areas */
    uint64_t used;    /**< block bytes minus free space inside the blocks */
    uint64_t free;    /**< free space inside allocated blocks */
    uint64_t foot;    /**< footprint: sum of allocated block lengths */
    uint64_t largest; /**< largest allocated block */
    uint32_t lo, hi;  /**< address range of the allocated blocks */
    unsigned blocks;  /**< number of allocated blocks */
};

/** @brief A region descriptor (an unallocated range, or a PVT region). */
struct vsm_region {
    uint32_t addr;
    uint32_t len;
};

/** @brief Everything one area reports, accumulated over continuation calls. */
struct vsm_info {
    unsigned area;  /**< enum vsm_area */
    unsigned space; /**< enum vsm_space of the last parse */
    unsigned tcbs;  /**< PVT: TCBs listed */
    int nsub;
    struct vsm_subpool sub[VSM_MAX_SUB];

    unsigned blocks; /**< allocated blocks in the area */
    uint64_t used;
    uint64_t free;
    uint64_t foot;
    uint64_t largest;
    uint32_t lo, hi;

    unsigned unalloc_blocks;
    uint64_t unalloc; /**< unallocated bytes (CSA/PVT only) */
    uint64_t largest_unalloc;
    int nregion;
    struct vsm_region region[VSM_MAX_REGION];

    bool truncated; /**< more subpools than VSM_MAX_SUB; totals still right */
    bool unalloc_valid; /**< the UNALLOC request succeeded */
    char err[160];      /**< why the last parse or service call failed */
};

/** @brief Zero @p v and set the request it describes. */
void vsm_reset(struct vsm_info *v, unsigned area, unsigned space);

/**
 * @brief Check the work-area header and locate the data area.
 *
 * Bytes 0-3 are the service return code, 4-7 the address of the data area,
 * 8-11 its length.  The data area must lie inside @p work.
 *
 * @return false when the header cannot be believed.
 */
bool vsm_work_data(const unsigned char *work, unsigned wlen,
                   const unsigned char **data, unsigned *len);

/**
 * @brief Parse an allocated-storage data area (SPACE=ALLOC or FREE).
 *
 * Merges into @p v, so a caller walking a return code 4 continuation simply
 * calls this again for each piece.  For PVT the block is the per-TCB form.
 *
 * @return false on a stream that does not match the documented layout, with
 *         v::err set and the offset in it.
 */
bool vsm_parse_alloc(struct vsm_info *v, const unsigned char *data,
                     unsigned len);

/** @brief Parse an unallocated-storage data area (CSA or PVT only). */
bool vsm_parse_unalloc(struct vsm_info *v, const unsigned char *data,
                       unsigned len);

#endif
