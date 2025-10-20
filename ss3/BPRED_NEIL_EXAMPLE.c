/*
 * EXAMPLE IMPLEMENTATION: BPredNeil as a Gshare Predictor
 *
 * This is a complete, working example showing how to implement BPredNeil
 * as a simple Gshare (global history with shared pattern table) predictor.
 *
 * Copy these code snippets into the appropriate locations in bpred.h and bpred.c
 */

// ===========================================================================
// STEP 1: Add to bpred.h - in the bpred_dir_t union (around line 130)
// ===========================================================================

struct bpred_dir_t
{
    enum bpred_class class;
    union
    {
        struct
        {
            unsigned int size;
            unsigned char *table;
        } bimod;

        struct
        {
            int l1size;
            int l2size;
            int shift_width;
            int xor;
            int *shiftregs;
            unsigned char *l2table;
        } two;

        // ADD THIS NEW STRUCTURE FOR BPREDNEIL
        struct
        {
            unsigned int table_size;     /* Number of prediction counters */
            unsigned int history_bits;   /* Number of global history bits */
            unsigned int global_history; /* Global history register */
            unsigned char *pred_table;   /* Prediction counter table */
        } neil;

    } config;
};

// ===========================================================================
// STEP 2: Modify bpred_create() in bpred.c (around line 86)
// ===========================================================================

struct bpred_t *bpred_create(...)
{
    struct bpred_t *pred;

    if (!(pred = calloc(1, sizeof(struct bpred_t))))
        fatal("out of virtual memory");

    pred->class = class;

    switch (class)
    {
    case BPredNeil:
        /* Neil's Gshare predictor implementation */
        /* Use bimod_size as pattern table size, shift_width as history length */
        pred->dirpred.bimod = bpred_dir_create(BPredNeil, bimod_size, 0, shift_width, 0);
        break;

    case BPredComb:
        /* bimodal component */
        pred->dirpred.bimod = bpred_dir_create(BPred2bit, bimod_size, 0, 0, 0);
        /* 2-level component */
        pred->dirpred.twolev = bpred_dir_create(BPred2Level, l1size, l2size, shift_width, xor);
        /* metapredictor component */
        pred->dirpred.meta = bpred_dir_create(BPred2bit, meta_size, 0, 0, 0);
        break;

        // ... rest of switch cases

        /* allocate ret-addr stack */
        switch (class)
        {
        case BPredNeil: // ADD THIS LINE
        case BPredComb:
        case BPred2Level:
        case BPred2bit:
        {
            int i;
            /* allocate BTB */
            // ... BTB allocation code (already exists)
            /* allocate retstack */
            // ... RAS allocation code (already exists)
            break;
        }
            // ... rest of cases
        }

        // ===========================================================================
        // STEP 3: Modify bpred_dir_create() in bpred.c (around line 184)
        // ===========================================================================

        struct bpred_dir_t *bpred_dir_create(
            enum bpred_class class,
            unsigned int l1size,
            unsigned int l2size,
            unsigned int shift_width,
            unsigned int xor)
        {
            struct bpred_dir_t *pred_dir;
            unsigned int cnt;
            int flipflop;

            if (!(pred_dir = calloc(1, sizeof(struct bpred_dir_t))))
                fatal("out of virtual memory");

            pred_dir->class = class;

            cnt = -1;
            switch (class)
            {
            case BPredNeil:
                /* Neil's Gshare predictor initialization */
                if (!l1size || (l1size & (l1size - 1)) != 0)
                    fatal("Neil predictor table size, `%d', must be non-zero and a power of two", l1size);

                if (!shift_width || shift_width > 20)
                    fatal("Neil predictor history width, `%d', must be between 1 and 20", shift_width);

                pred_dir->config.neil.table_size = l1size;
                pred_dir->config.neil.history_bits = shift_width;
                pred_dir->config.neil.global_history = 0; /* Initialize to all zeros */

                /* Allocate prediction table */
                pred_dir->config.neil.pred_table = calloc(l1size, sizeof(unsigned char));
                if (!pred_dir->config.neil.pred_table)
                    fatal("cannot allocate Neil predictor table");

                /* Initialize counters to weakly taken/not-taken (alternating 1 and 2) */
                flipflop = 1;
                for (cnt = 0; cnt < l1size; cnt++)
                {
                    pred_dir->config.neil.pred_table[cnt] = flipflop;
                    flipflop = 3 - flipflop; /* Toggle between 1 and 2 */
                }

                break;

            case BPred2Level:
                // ... existing code
                break;

            case BPred2bit:
                // ... existing code
                break;

                // ... rest of cases
            }

            return pred_dir;
        }

        // ===========================================================================
        // STEP 4: Modify bpred_dir_config() in bpred.c (around line 272)
        // ===========================================================================

        void bpred_dir_config(
            struct bpred_dir_t * pred_dir,
            char name[],
            FILE *stream)
        {
            switch (pred_dir->class)
            {
            case BPredNeil:
                fprintf(stream,
                        "pred_dir: %s: Neil's Gshare: %d entries, %d-bit global history\n",
                        name,
                        pred_dir->config.neil.table_size,
                        pred_dir->config.neil.history_bits);
                break;

            case BPred2Level:
                // ... existing code
                break;

                // ... rest of cases
            }
        }

        // ===========================================================================
        // STEP 5: Modify bpred_config() in bpred.c (around line 305)
        // ===========================================================================

        void bpred_config(struct bpred_t * pred, FILE * stream)
        {
            switch (pred->class)
            {
            case BPredNeil:
                bpred_dir_config(pred->dirpred.bimod, "neil_gshare", stream);
                fprintf(stream, "btb: %d sets x %d associativity\n",
                        pred->btb.sets, pred->btb.assoc);
                fprintf(stream, "ret_stack: %d entries\n", pred->retstack.size);
                break;

            case BPredComb:
                // ... existing code
                break;

                // ... rest of cases
            }
        }

        // ===========================================================================
        // STEP 6: Modify bpred_dir_lookup() in bpred.c (around line 498)
        // ===========================================================================

        char *bpred_dir_lookup(struct bpred_dir_t * pred_dir, md_addr_t baddr)
        {
            unsigned char *p = NULL;

            switch (pred_dir->class)
            {
            case BPredNeil:
            {
                /* Gshare: XOR branch address with global history */
                unsigned int pc_bits = (baddr >> MD_BR_SHIFT);
                unsigned int history = pred_dir->config.neil.global_history;
                unsigned int index;

                /* XOR PC with history to get index */
                index = pc_bits ^ history;

                /* Mask to table size */
                index = index & (pred_dir->config.neil.table_size - 1);

                /* Return pointer to prediction counter */
                p = &pred_dir->config.neil.pred_table[index];
            }
            break;

            case BPred2Level:
                // ... existing code
                break;

            case BPred2bit:
                // ... existing code
                break;

                // ... rest of cases
            }

            return (char *)p;
        }

        // ===========================================================================
        // STEP 7: Modify bpred_lookup() in bpred.c (around line 587)
        // ===========================================================================

        md_addr_t bpred_lookup(
            struct bpred_t * pred,
            md_addr_t baddr,
            md_addr_t btarget,
            enum md_opcode op,
            int is_call,
            int is_return,
            struct bpred_update_t *dir_update_ptr,
            int *stack_recover_idx)
        {
            struct bpred_btb_ent_t *pbtb = NULL;
            int index, i;

            if (!dir_update_ptr)
                panic("no bpred update record");

            if (!(MD_OP_FLAGS(op) & F_CTRL))
                return 0;

            pred->lookups++;

            dir_update_ptr->dir.ras = FALSE;
            dir_update_ptr->pdir1 = NULL;
            dir_update_ptr->pdir2 = NULL;
            dir_update_ptr->pmeta = NULL;

            switch (pred->class)
            {
            case BPredNeil:
                /* For conditional branches, get prediction */
                if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND))
                {
                    /* Lookup in Gshare predictor */
                    dir_update_ptr->pdir1 = bpred_dir_lookup(pred->dirpred.bimod, baddr);

                    /* No secondary predictor or meta predictor for Gshare */
                    dir_update_ptr->pdir2 = NULL;
                    dir_update_ptr->pmeta = NULL;
                }
                break;

            case BPredComb:
                // ... existing code
                break;

                // ... rest of cases
            }

            /* RAS and BTB handling - common code (already exists) */
            // ... Rest of function remains the same
        }

        // ===========================================================================
        // STEP 8: Modify bpred_update() in bpred.c (around line 835)
        // ===========================================================================

        void bpred_update(
            struct bpred_t * pred,
            md_addr_t baddr,
            md_addr_t btarget,
            int taken,
            int pred_taken,
            int correct,
            enum md_opcode op,
            struct bpred_update_t *dir_update_ptr)
        {
            struct bpred_btb_ent_t *pbtb = NULL;
            struct bpred_btb_ent_t *lruhead = NULL, *lruitem = NULL;
            int index, i;

            if (!(MD_OP_FLAGS(op) & F_CTRL))
                return;

            /* Update statistics - common code (already exists) */
            // ... statistics update

            /* Can exit now if this is a stateless predictor */
            if (pred->class == BPredNotTaken || pred->class == BPredTaken)
                return;

            /* Update global history for BPredNeil */
            if (pred->class == BPredNeil &&
                (MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND))
            {
                unsigned int new_history;

                /* Shift history left and insert new outcome */
                new_history = (pred->dirpred.bimod->config.neil.global_history << 1) | (!!taken);

                /* Mask to history length */
                new_history = new_history & ((1 << pred->dirpred.bimod->config.neil.history_bits) - 1);

                /* Update global history register */
                pred->dirpred.bimod->config.neil.global_history = new_history;
            }

            /* Update L1 table if appropriate for 2-level predictors */
            if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND) &&
                (pred->class == BPred2Level || pred->class == BPredComb))
            {
                // ... existing 2-level history update code
            }

            /* Update direction counters - standard saturating counter logic */
            if (dir_update_ptr->pdir1)
            {
                if (taken)
                {
                    if (*dir_update_ptr->pdir1 < 3)
                        ++*dir_update_ptr->pdir1; /* Increment towards strongly taken */
                }
                else
                {
                    if (*dir_update_ptr->pdir1 > 0)
                        --*dir_update_ptr->pdir1; /* Decrement towards strongly not-taken */
                }
            }

            /* Rest of function for pdir2, pmeta, BTB update - common code (already exists) */
            // ...
        }

    // ===========================================================================
    // USAGE EXAMPLE
    // ===========================================================================

    /*
     * To compile:
     *   make clean
     *   make sim-outorder
     *
     * To run with BPredNeil (Gshare with 4096 entries, 12-bit history):
     *   ./sim-outorder -bpred neil -bpred:neil 4096 1 1 1 12 0 2048 1 8 tests-pisa/bin.little/test-math
     *
     * Parameters explanation for -bpred:neil:
     *   4096  - Pattern table size (bimod_size, used for our Gshare table)
     *   1     - l1size (not used for Gshare, but required parameter)
     *   1     - l2size (not used for Gshare, but required parameter)
     *   1     - meta_size (not used for Gshare, but required parameter)
     *   12    - History bits (shift_width, used for global history length)
     *   0     - XOR flag (not used for Gshare, but required parameter)
     *   2048  - BTB sets
     *   1     - BTB associativity
     *   8     - Return address stack size
     *
     * To see statistics:
     *   Look for lines starting with "bpred_neil." in the output
     */

    // ===========================================================================
    // UNDERSTANDING THE GSHARE ALGORITHM
    // ===========================================================================

    /*
     * Gshare Predictor:
     *
     * 1. Single global history register (GHR)
     *    - Tracks last N branch outcomes (taken=1, not-taken=0)
     *    - Updated after each branch: GHR = (GHR << 1) | outcome
     *
     * 2. Pattern History Table (PHT)
     *    - Array of 2-bit saturating counters
     *    - Indexed by: PC XOR GHR
     *
     * 3. On lookup:
     *    index = (PC >> 2) XOR global_history
     *    index = index & (table_size - 1)
     *    prediction = (PHT[index] >= 2) ? TAKEN : NOT_TAKEN
     *
     * 4. On update:
     *    Update global_history: GHR = (GHR << 1) | actual_outcome
     *    Update counter: increment if taken, decrement if not-taken
     *
     * Benefits:
     *    - Correlates branch behavior with recent global history
     *    - XORing shares PHT entries among correlated branches
     *    - Simple and effective for many workloads
     */

    // ===========================================================================
    // DEBUGGING TIPS
    // ===========================================================================

    /*
     * 1. Add debug prints in bpred_dir_lookup():
     *    fprintf(stderr, "Neil lookup: addr=0x%x, history=%x, index=%d, counter=%d\n",
     *            baddr, history, index, *p);
     *
     * 2. Add debug prints in bpred_update():
     *    fprintf(stderr, "Neil update: addr=0x%x, taken=%d, old_counter=%d, new_counter=%d, history=%x\n",
     *            baddr, taken, old_val, *dir_update_ptr->pdir1,
     *            pred->dirpred.bimod->config.neil.global_history);
     *
     * 3. Check that pointers are preserved:
     *    - Pointer stored in lookup must be same one used in update
     *    - Use assert() to verify
     *
     * 4. Verify table indices are in bounds:
     *    assert(index < pred_dir->config.neil.table_size);
     */

    // ===========================================================================
    // NEXT STEPS: CUSTOMIZE YOUR PREDICTOR
    // ===========================================================================

    /*
     * Once you have Gshare working, you can modify it:
     *
     * 1. Change the indexing function:
     *    - Try different hash functions
     *    - Use more/fewer PC bits
     *    - Try different XOR patterns
     *
     * 2. Use different counter schemes:
     *    - 3-bit counters instead of 2-bit
     *    - Hysteresis counters
     *    - Confidence counters
     *
     * 3. Add multiple history lengths:
     *    - Keep multiple GHRs of different lengths
     *    - Select best one dynamically
     *
     * 4. Implement a completely different algorithm:
     *    - Perceptron predictor
     *    - TAGE predictor
     *    - Neural predictor
     *
     * The framework is flexible - you just need to:
     *    - Define your state in the neil struct
     *    - Initialize it in bpred_dir_create()
     *    - Lookup state in bpred_dir_lookup()
     *    - Update state in bpred_update()
     */
