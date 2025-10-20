# Branch Predictor Implementation Guide for BPredNeil

## Table of Contents
1. [Overview](#overview)
2. [Key Data Structures](#key-data-structures)
3. [Function Deep Dive](#function-deep-dive)
4. [How to Implement BPredNeil](#how-to-implement-bpredneil)
5. [Step-by-Step Implementation Guide](#step-by-step-implementation-guide)

---

## Overview

The SimpleScalar branch predictor framework supports multiple branch prediction schemes. Currently, BPredNeil is defined but uses the same logic as BPredComb (combined predictor). This guide will help you implement your own custom branch prediction logic.

### Current Predictor Types:
- **BPredComb**: Combined predictor (uses bimodal + 2-level + meta predictor)
- **BPred2Level**: 2-level correlating predictor with 2-bit counters
- **BPred2bit**: Simple 2-bit saturating counter predictor
- **BPredTaken**: Static predict taken
- **BPredNotTaken**: Static predict not taken
- **BPredNeil**: Your custom predictor (currently defaults to BPredComb)

---

## Key Data Structures

### 1. `struct bpred_t` - Main Branch Predictor Structure

```c
struct bpred_t {
    enum bpred_class class;         // Type of predictor
    
    struct {
        struct bpred_dir_t *bimod;  // Bimodal direction predictor
        struct bpred_dir_t *twolev; // 2-level direction predictor
        struct bpred_dir_t *meta;   // Meta predictor (chooses between bimod/2lev)
    } dirpred;
    
    struct {
        int sets;                          // Number of BTB sets
        int assoc;                         // BTB associativity
        struct bpred_btb_ent_t *btb_data; // BTB table
    } btb;
    
    struct {
        int size;                      // Return-address stack size
        int tos;                       // Top-of-stack pointer
        struct bpred_btb_ent_t *stack; // RAS entries
    } retstack;
    
    // Statistics
    counter_t addr_hits;      // Correct address predictions
    counter_t dir_hits;       // Correct direction predictions
    counter_t misses;         // Incorrect predictions
    counter_t used_ras;       // Times RAS was used
    counter_t used_bimod;     // Times bimodal was used
    counter_t used_2lev;      // Times 2-level was used
    counter_t lookups;        // Total lookups
    // ... more stats
};
```

### 2. `struct bpred_dir_t` - Direction Predictor Structure

```c
struct bpred_dir_t {
    enum bpred_class class;  // Predictor type
    
    union {
        // For bimodal predictor (BPred2bit)
        struct {
            unsigned int size;         // Number of table entries
            unsigned char *table;      // 2-bit saturating counters
        } bimod;
        
        // For 2-level predictor (BPred2Level)
        struct {
            int l1size;                // Level-1 size (# of history registers)
            int l2size;                // Level-2 size (# of counters)
            int shift_width;           // History register width (bits)
            int xor;                   // XOR address with history?
            int *shiftregs;            // Level-1 history table
            unsigned char *l2table;    // Level-2 prediction state table
        } two;
    } config;
};
```

**2-bit Saturating Counter States:**
- `0`: Strongly Not Taken
- `1`: Weakly Not Taken
- `2`: Weakly Taken
- `3`: Strongly Taken

### 3. `struct bpred_update_t` - Update Information

```c
struct bpred_update_t {
    char *pdir1;  // Pointer to primary direction predictor counter
    char *pdir2;  // Pointer to secondary direction predictor counter
    char *pmeta;  // Pointer to meta predictor counter
    
    struct {
        unsigned int ras : 1;     // Was RAS used?
        unsigned int bimod : 1;   // Bimodal prediction
        unsigned int twolev : 1;  // 2-level prediction
        unsigned int meta : 1;    // Meta predictor choice (0=bimod, 1=2lev)
    } dir;
};
```

### 4. `struct bpred_btb_ent_t` - BTB Entry

```c
struct bpred_btb_ent_t {
    md_addr_t addr;                       // Branch PC address
    enum md_opcode op;                    // Branch opcode
    md_addr_t target;                     // Target address
    struct bpred_btb_ent_t *prev, *next; // LRU chain pointers
};
```

---

## Function Deep Dive

### 1. `bpred_create()` - Creating the Predictor

**Purpose:** Allocates and initializes all predictor structures.

**Called:** Once during simulator initialization (in `sim-outorder.c` or `sim-bpred.c`)

**What it does:**
1. Allocates main `bpred_t` structure
2. Creates direction predictors based on type (bimodal, 2-level, meta)
3. Allocates BTB (Branch Target Buffer)
4. Allocates RAS (Return Address Stack)
5. Initializes all counters and tables

**Key Code Section:**
```c
case BPredComb:
    // Create bimodal component
    pred->dirpred.bimod = bpred_dir_create(BPred2bit, bimod_size, 0, 0, 0);
    
    // Create 2-level component
    pred->dirpred.twolev = bpred_dir_create(BPred2Level, l1size, l2size, shift_width, xor);
    
    // Create meta predictor
    pred->dirpred.meta = bpred_dir_create(BPred2bit, meta_size, 0, 0, 0);
    break;
```

**For BPredNeil:** Currently falls through to BPredComb. You need to add your own initialization here.

---

### 2. `bpred_dir_create()` - Creating Direction Predictor

**Purpose:** Creates a specific direction predictor (bimodal or 2-level)

**What it does:**

**For BPred2bit (Bimodal):**
```c
pred_dir->config.bimod.size = l1size;
pred_dir->config.bimod.table = calloc(l1size, sizeof(unsigned char));

// Initialize counters to alternating weak states (1 and 2)
for (cnt = 0; cnt < l1size; cnt++) {
    pred_dir->config.bimod.table[cnt] = flipflop;  // 1 or 2
    flipflop = 3 - flipflop;  // Toggles between 1 and 2
}
```

**For BPred2Level:**
```c
pred_dir->config.two.l1size = l1size;         // # of history registers
pred_dir->config.two.l2size = l2size;         // # of pattern counters
pred_dir->config.two.shift_width = shift_width;  // History bits
pred_dir->config.two.shiftregs = calloc(l1size, sizeof(int));
pred_dir->config.two.l2table = calloc(l2size, sizeof(unsigned char));
```

---

### 3. `bpred_lookup()` - Predicting Branch Direction and Target

**Purpose:** Called during FETCH stage to predict next PC for a branch

**Parameters:**
- `baddr`: Branch PC address
- `btarget`: Target address (if known, 0 otherwise)
- `op`: Branch opcode
- `is_call`: Is this a function call?
- `is_return`: Is this a return?
- `dir_update_ptr`: Output - stores predictor state for later update
- `stack_recover_idx`: Output - RAS state for mispredict recovery

**Returns:** 
- **0** = not taken (fetch next sequential instruction)
- **1** = taken but target unknown (BTB miss)
- **target address** = taken with known target

**Flow for BPredComb:**

```c
case BPredComb:
    if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND)) {
        // For conditional branches
        
        // 1. Lookup in bimodal predictor
        char *bimod = bpred_dir_lookup(pred->dirpred.bimod, baddr);
        
        // 2. Lookup in 2-level predictor
        char *twolev = bpred_dir_lookup(pred->dirpred.twolev, baddr);
        
        // 3. Lookup in meta predictor
        char *meta = bpred_dir_lookup(pred->dirpred.meta, baddr);
        
        // 4. Store pointers for update
        dir_update_ptr->pmeta = meta;
        dir_update_ptr->dir.meta = (*meta >= 2);      // Meta choice
        dir_update_ptr->dir.bimod = (*bimod >= 2);    // Bimod prediction
        dir_update_ptr->dir.twolev = (*twolev >= 2);  // 2-lev prediction
        
        // 5. Select primary predictor based on meta
        if (*meta >= 2) {
            // Use 2-level predictor
            dir_update_ptr->pdir1 = twolev;
            dir_update_ptr->pdir2 = bimod;
        } else {
            // Use bimodal predictor
            dir_update_ptr->pdir1 = bimod;
            dir_update_ptr->pdir2 = twolev;
        }
    }
    break;
```

**RAS Handling:**
```c
// For returns: pop from RAS
if (is_return && pred->retstack.size) {
    md_addr_t target = pred->retstack.stack[pred->retstack.tos].target;
    pred->retstack.tos = (pred->retstack.tos + pred->retstack.size - 1) % pred->retstack.size;
    pred->retstack_pops++;
    dir_update_ptr->dir.ras = TRUE;
    return target;
}

// For calls: push return address onto RAS
if (is_call && pred->retstack.size) {
    pred->retstack.tos = (pred->retstack.tos + 1) % pred->retstack.size;
    pred->retstack.stack[pred->retstack.tos].target = baddr + sizeof(md_inst_t);
    pred->retstack_pushes++;
}
```

**BTB Lookup:**
```c
// Search BTB for this branch
index = (baddr >> MD_BR_SHIFT) & (pred->btb.sets - 1);

// Search for matching entry
for (i = index; i < (index + pred->btb.assoc); i++) {
    if (pred->btb.btb_data[i].addr == baddr) {
        pbtb = &pred->btb.btb_data[i];  // BTB hit
        break;
    }
}

// Return prediction
if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) == (F_CTRL | F_UNCOND)) {
    // Unconditional branch/jump
    return (pbtb ? pbtb->target : 1);
} else {
    // Conditional branch
    if (pbtb == NULL) {
        // BTB miss - return direction only
        return ((*(dir_update_ptr->pdir1) >= 2) ? 1 : 0);
    } else {
        // BTB hit - return target if predicted taken
        return ((*(dir_update_ptr->pdir1) >= 2) ? pbtb->target : 0);
    }
}
```

---

### 4. `bpred_dir_lookup()` - Looking Up Direction Predictor

**Purpose:** Gets pointer to the counter for this branch in the direction predictor

**For Bimodal (BPred2bit):**
```c
// Hash function: XOR high bits with low bits
#define BIMOD_HASH(PRED, ADDR) \
    ((((ADDR) >> 19) ^ ((ADDR) >> MD_BR_SHIFT)) & ((PRED)->config.bimod.size - 1))

case BPred2bit:
    p = &pred_dir->config.bimod.table[BIMOD_HASH(pred_dir, baddr)];
    break;
```

**For 2-Level:**
```c
case BPred2Level:
    // 1. Get Level-1 index from branch address
    l1index = (baddr >> MD_BR_SHIFT) & (pred_dir->config.two.l1size - 1);
    
    // 2. Get history register value
    l2index = pred_dir->config.two.shiftregs[l1index];
    
    // 3. Combine with address (optionally with XOR)
    if (pred_dir->config.two.xor) {
        l2index = (((l2index ^ (baddr >> MD_BR_SHIFT)) 
                    & ((1 << pred_dir->config.two.shift_width) - 1))
                   | ((baddr >> MD_BR_SHIFT) << pred_dir->config.two.shift_width));
    } else {
        l2index = l2index | ((baddr >> MD_BR_SHIFT) << pred_dir->config.two.shift_width);
    }
    
    // 4. Mask to table size
    l2index = l2index & (pred_dir->config.two.l2size - 1);
    
    // 5. Get pointer to counter
    p = &pred_dir->config.two.l2table[l2index];
    break;
```

---

### 5. `bpred_update()` - Updating Predictor After Branch Resolution

**Purpose:** Called after branch executes to update predictor state

**Called:** In DECODE, WRITEBACK, or COMMIT stage (depending on `bpred_spec_update` config)

**Parameters:**
- `baddr`: Branch address
- `btarget`: Actual target address
- `taken`: Was branch actually taken?
- `pred_taken`: Was branch predicted taken?
- `correct`: Was the prediction correct?
- `op`: Branch opcode
- `dir_update_ptr`: Predictor state from lookup

**What it updates:**

**1. Statistics:**
```c
if (correct)
    pred->addr_hits++;     // Correct target prediction

if (!!pred_taken == !!taken)
    pred->dir_hits++;      // Correct direction prediction
else
    pred->misses++;        // Misprediction
```

**2. History Registers (for 2-level predictors):**
```c
if (pred->class == BPred2Level || pred->class == BPredComb || pred->class == BPredNeil) {
    // Update Level-1 history register
    l1index = (baddr >> MD_BR_SHIFT) & (pred->dirpred.twolev->config.two.l1size - 1);
    
    // Shift in the actual outcome (taken=1, not-taken=0)
    shift_reg = (pred->dirpred.twolev->config.two.shiftregs[l1index] << 1) | (!!taken);
    
    // Keep only the last 'shift_width' bits
    pred->dirpred.twolev->config.two.shiftregs[l1index] = 
        shift_reg & ((1 << pred->dirpred.twolev->config.two.shift_width) - 1);
}
```

**3. Direction Counters (2-bit saturating):**
```c
// Update primary predictor counter
if (dir_update_ptr->pdir1) {
    if (taken) {
        if (*dir_update_ptr->pdir1 < 3)
            ++*dir_update_ptr->pdir1;  // Increment (max 3)
    } else {
        if (*dir_update_ptr->pdir1 > 0)
            --*dir_update_ptr->pdir1;  // Decrement (min 0)
    }
}

// Update secondary predictor (for combined predictor)
if (dir_update_ptr->pdir2) {
    if (taken) {
        if (*dir_update_ptr->pdir2 < 3)
            ++*dir_update_ptr->pdir2;
    } else {
        if (*dir_update_ptr->pdir2 > 0)
            --*dir_update_ptr->pdir2;
    }
}
```

**4. Meta Predictor (for combined predictor):**
```c
if (dir_update_ptr->pmeta) {
    // Only update if bimodal and 2-level disagreed
    if (dir_update_ptr->dir.bimod != dir_update_ptr->dir.twolev) {
        if (dir_update_ptr->dir.twolev == (unsigned int)taken) {
            // 2-level was correct
            if (*dir_update_ptr->pmeta < 3)
                ++*dir_update_ptr->pmeta;  // Favor 2-level
        } else {
            // Bimodal was correct
            if (*dir_update_ptr->pmeta > 0)
                --*dir_update_ptr->pmeta;  // Favor bimodal
        }
    }
}
```

**5. BTB (Branch Target Buffer):**
```c
if (taken) {
    // Find/allocate BTB entry
    index = (baddr >> MD_BR_SHIFT) & (pred->btb.sets - 1);
    
    // Search for existing entry or find LRU victim
    // ... (LRU search logic)
    
    if (pbtb) {
        if (pbtb->addr == baddr) {
            // Update existing entry
            if (!correct)
                pbtb->target = btarget;
        } else {
            // Allocate new entry
            pbtb->addr = baddr;
            pbtb->op = op;
            pbtb->target = btarget;
        }
    }
}
```

---

### 6. `bpred_recover()` - Recovering from Misprediction

**Purpose:** Restores RAS to correct state after misprediction

**Called:** When branch misprediction is detected in execute/writeback

```c
void bpred_recover(struct bpred_t *pred, md_addr_t baddr, int stack_recover_idx) {
    if (pred == NULL)
        return;
    
    // Restore return-address stack to pre-speculation state
    pred->retstack.tos = stack_recover_idx;
}
```

---

## How to Implement BPredNeil

### Understanding the Current Flow

Currently, BPredNeil falls through to BPredComb in all functions:

**In `bpred_create()`:**
```c
case BPredNeil:
    /* Neil's predictor not implemented yet */
case BPredComb:
    // Uses combined predictor logic
```

**In `bpred_lookup()`:**
```c
case BPredNeil:
    /* Neil's predictor not implemented yet */
case BPredComb:
    // Uses combined predictor logic
```

---

## Step-by-Step Implementation Guide

### Example: Implementing a Perceptron-Based Predictor

Let's implement BPredNeil as a simple example. I'll show you how to add a custom predictor structure and logic.

### Step 1: Define Your Predictor Structure

**Add to `bpred.h` in the `bpred_dir_t` union:**

```c
struct bpred_dir_t {
    enum bpred_class class;
    union {
        struct {
            unsigned int size;
            unsigned char *table;
        } bimod;
        
        struct {
            int l1size;
            int l2size;
            int shift_width;
            int xor;
            int *shiftregs;
            unsigned char *l2table;
        } two;
        
        // ADD YOUR CUSTOM STRUCTURE HERE
        struct {
            unsigned int num_entries;      // Number of predictor entries
            unsigned int history_length;   // Global history length
            int global_history;            // Global history register
            // Add your custom tables/structures
            unsigned char *custom_table;   // Example: custom prediction table
        } neil;  // Your custom predictor
    } config;
};
```

### Step 2: Initialize in `bpred_create()`

**In `bpred.c`, modify `bpred_create()`:**

```c
case BPredNeil:
    // Create your custom predictor components
    pred->dirpred.bimod = bpred_dir_create(BPredNeil, bimod_size, l1size, shift_width, xor);
    
    // You can still use BTB and RAS
    // They will be allocated in the common code below
    break;

case BPredComb:
    // ... existing BPredComb code
```

### Step 3: Initialize Direction Predictor in `bpred_dir_create()`

**In `bpred.c`, add case in `bpred_dir_create()`:**

```c
struct bpred_dir_t *bpred_dir_create(...) {
    struct bpred_dir_t *pred_dir;
    
    pred_dir = calloc(1, sizeof(struct bpred_dir_t));
    pred_dir->class = class;
    
    switch (class) {
        case BPred2Level:
            // ... existing code
            break;
            
        case BPred2bit:
            // ... existing code
            break;
            
        case BPredNeil:
            // Initialize your predictor
            if (!l1size || (l1size & (l1size - 1)) != 0)
                fatal("Neil predictor size must be power of 2");
            
            pred_dir->config.neil.num_entries = l1size;
            pred_dir->config.neil.history_length = shift_width;
            pred_dir->config.neil.global_history = 0;
            
            // Allocate your tables
            pred_dir->config.neil.custom_table = 
                calloc(l1size, sizeof(unsigned char));
            if (!pred_dir->config.neil.custom_table)
                fatal("cannot allocate Neil predictor table");
            
            // Initialize your table
            for (int i = 0; i < l1size; i++) {
                pred_dir->config.neil.custom_table[i] = 1;  // Initialize as needed
            }
            break;
            
        default:
            panic("bogus predictor class");
    }
    
    return pred_dir;
}
```

### Step 4: Implement Lookup Logic in `bpred_dir_lookup()`

**In `bpred.c`, add case in `bpred_dir_lookup()`:**

```c
char *bpred_dir_lookup(struct bpred_dir_t *pred_dir, md_addr_t baddr) {
    unsigned char *p = NULL;
    
    switch (pred_dir->class) {
        case BPred2Level:
            // ... existing code
            break;
            
        case BPred2bit:
            // ... existing code
            break;
            
        case BPredNeil:
            // YOUR CUSTOM LOOKUP LOGIC
            // Example: hash address with global history
            int index = (baddr >> MD_BR_SHIFT) ^ pred_dir->config.neil.global_history;
            index = index & (pred_dir->config.neil.num_entries - 1);
            p = &pred_dir->config.neil.custom_table[index];
            break;
            
        default:
            panic("bogus predictor class");
    }
    
    return (char *)p;
}
```

### Step 5: Implement Prediction in `bpred_lookup()`

**In `bpred.c`, modify `bpred_lookup()`:**

```c
md_addr_t bpred_lookup(struct bpred_t *pred, ...) {
    // ... initial setup code
    
    switch (pred->class) {
        case BPredNeil:
            if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND)) {
                // For conditional branches
                
                // Get pointer to your predictor state
                dir_update_ptr->pdir1 = bpred_dir_lookup(pred->dirpred.bimod, baddr);
                
                // Store any additional state needed for update
                // Use dir_update_ptr->dir fields as needed
            }
            break;
            
        case BPredComb:
            // ... existing code
            break;
    }
    
    // ... rest of function (RAS, BTB lookup, return prediction)
}
```

### Step 6: Implement Update Logic in `bpred_update()`

**In `bpred.c`, modify `bpred_update()`:**

```c
void bpred_update(struct bpred_t *pred, ...) {
    // ... statistics update code
    
    // Update history (if applicable)
    if (pred->class == BPredNeil) {
        // Update your global history register
        pred->dirpred.bimod->config.neil.global_history = 
            (pred->dirpred.bimod->config.neil.global_history << 1) | (!!taken);
        
        // Mask to history length
        pred->dirpred.bimod->config.neil.global_history &= 
            ((1 << pred->dirpred.bimod->config.neil.history_length) - 1);
    }
    
    // Standard counter update (or implement your own)
    if (dir_update_ptr->pdir1) {
        // Update your prediction table
        // Example: 2-bit saturating counter
        if (taken) {
            if (*dir_update_ptr->pdir1 < 3)
                ++*dir_update_ptr->pdir1;
        } else {
            if (*dir_update_ptr->pdir1 > 0)
                --*dir_update_ptr->pdir1;
        }
        
        // OR implement your own custom update logic
    }
    
    // ... BTB update code (usually can reuse)
}
```

### Step 7: Add Configuration and Stats

**In `bpred.c`, update `bpred_config()`:**

```c
void bpred_config(struct bpred_t *pred, FILE *stream) {
    switch (pred->class) {
        case BPredNeil:
            fprintf(stream, "pred_dir: neil: custom predictor\n");
            fprintf(stream, "  entries: %d\n", 
                    pred->dirpred.bimod->config.neil.num_entries);
            fprintf(stream, "  history_length: %d\n", 
                    pred->dirpred.bimod->config.neil.history_length);
            fprintf(stream, "btb: %d sets x %d associativity\n",
                    pred->btb.sets, pred->btb.assoc);
            fprintf(stream, "ret_stack: %d entries\n", pred->retstack.size);
            break;
            
        case BPredComb:
            // ... existing code
            break;
    }
}
```

### Step 8: Add to `bpred_dir_config()`

**In `bpred.c`:**

```c
void bpred_dir_config(struct bpred_dir_t *pred_dir, char name[], FILE *stream) {
    switch (pred_dir->class) {
        case BPredNeil:
            fprintf(stream, "pred_dir: %s: Neil's custom predictor\n", name);
            fprintf(stream, "  %d entries, %d-bit history\n",
                    pred_dir->config.neil.num_entries,
                    pred_dir->config.neil.history_length);
            break;
            
        // ... other cases
    }
}
```

---

## Common Predictor Patterns

### Pattern 1: Global History Predictor
- Single global history register
- Hash: `index = (PC ^ global_history) % table_size`
- Update history: `global_history = (global_history << 1) | taken`

### Pattern 2: Local History Predictor (PAg)
- Per-branch history registers
- Hash Level-1: `l1_idx = PC % l1_size`
- Hash Level-2: `l2_idx = history[l1_idx] % l2_size`

### Pattern 3: Perceptron Predictor
- Store weights instead of counters
- Compute dot product of history and weights
- Update weights based on mispredictions

### Pattern 4: Hybrid/Combined Predictor
- Multiple sub-predictors
- Meta-predictor chooses best sub-predictor
- Track which sub-predictor is more accurate

---

## Testing Your Predictor

### 1. Compile:
```bash
make clean
make sim-outorder
```

### 2. Test with simple program:
```bash
./sim-outorder -bpred neil -bpred:neil 512 8 512 0 8 0 2048 1 8 tests-pisa/bin.little/test-math
```

### 3. Check statistics:
Look for:
- `bpred_neil.dir_hits`: Correct predictions
- `bpred_neil.misses`: Mispredictions
- `bpred_neil.bpred_dir_rate`: Accuracy

---

## Key Points to Remember

1. **bpred_lookup() returns:**
   - 0 = not taken
   - 1 = taken (BTB miss)
   - target_address = taken (BTB hit)

2. **2-bit counter encoding:**
   - 0, 1 = predict NOT taken
   - 2, 3 = predict TAKEN

3. **Update must use pointers from lookup:**
   - Store pointers in `dir_update_ptr` during lookup
   - Use these same pointers during update

4. **RAS and BTB are optional:**
   - You can reuse existing RAS/BTB code
   - Focus on direction prediction logic

5. **Speculative updates:**
   - Updates can happen before branch resolves
   - Handle misprediction recovery with `bpred_recover()`

---

## Quick Checklist for BPredNeil Implementation

- [ ] Add custom structure to `bpred_dir_t` union in `bpred.h`
- [ ] Implement `case BPredNeil:` in `bpred_create()`
- [ ] Implement `case BPredNeil:` in `bpred_dir_create()`
- [ ] Implement `case BPredNeil:` in `bpred_dir_lookup()`
- [ ] Implement `case BPredNeil:` in `bpred_lookup()`
- [ ] Implement history/state update in `bpred_update()`
- [ ] Implement `case BPredNeil:` in `bpred_config()`
- [ ] Implement `case BPredNeil:` in `bpred_dir_config()`
- [ ] Add BPredNeil to `bpred_reg_stats()` if needed (already there)
- [ ] Test and debug

---

## Example: Simple Custom Predictor

Here's a complete minimal example - a simple always-taken predictor:

```c
// In bpred_dir_lookup() for BPredNeil:
case BPredNeil:
    // Always return a counter set to "strongly taken" (3)
    static unsigned char always_taken = 3;
    p = &always_taken;
    break;

// In bpred_update() for BPredNeil:
// No update needed for static predictor
```

Good luck implementing BPredNeil! Start simple and gradually add complexity.
