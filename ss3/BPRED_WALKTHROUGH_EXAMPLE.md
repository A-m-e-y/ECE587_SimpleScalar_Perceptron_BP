# Complete Step-by-Step Walkthrough: BPred2bit Example

## Scenario Setup

Let's trace what happens when we predict a simple branch instruction:

```
Branch at address: 0x00401000
Instruction: beq $1, $2, target
Target address: 0x00401100
```

We'll use a **BPred2bit** predictor with:
- **4096 entries** in the bimodal table
- **2048 sets** in BTB
- **8 entries** in RAS

---

## Phase 0: INITIALIZATION (Happens Once at Startup)

### Step 0.1: `bpred_create()` is called

```c
pred = bpred_create(BPred2bit,    // class
                    4096,         // bimod_size
                    0, 0, 0,      // l1size, l2size, meta_size (unused)
                    0, 0,         // shift_width, xor (unused)
                    2048,         // btb_sets
                    1,            // btb_assoc
                    8);           // retstack_size
```

**What happens inside `bpred_create()`:**

```c
// Allocate main structure
struct bpred_t *pred = calloc(1, sizeof(struct bpred_t));

// Memory layout after allocation:
pred = {
    .class = BPred2bit,
    .dirpred = {
        .bimod = NULL,     // Will be filled next
        .twolev = NULL,
        .meta = NULL
    },
    .btb = {
        .sets = 0,
        .assoc = 0,
        .btb_data = NULL
    },
    .retstack = {
        .size = 0,
        .tos = 0,
        .stack = NULL
    },
    // All stats initialized to 0
    .lookups = 0,
    .addr_hits = 0,
    .dir_hits = 0,
    .misses = 0,
    ...
};

// Now execute the switch case for BPred2bit:
switch (class) {
    case BPred2bit:
        // Create the bimodal direction predictor
        pred->dirpred.bimod = bpred_dir_create(BPred2bit, 4096, 0, 0, 0);
        break;
}
```

### Step 0.2: `bpred_dir_create()` is called

```c
bpred_dir_create(BPred2bit, 4096, 0, 0, 0);
```

**What happens inside:**

```c
// Allocate direction predictor structure
struct bpred_dir_t *pred_dir = calloc(1, sizeof(struct bpred_dir_t));

pred_dir->class = BPred2bit;

// Execute switch case:
case BPred2bit:
    pred_dir->config.bimod.size = 4096;
    
    // Allocate the actual table (array of 4096 bytes)
    pred_dir->config.bimod.table = calloc(4096, sizeof(unsigned char));
    
    // Initialize counters to weakly taken/not-taken
    int flipflop = 1;
    for (cnt = 0; cnt < 4096; cnt++) {
        pred_dir->config.bimod.table[cnt] = flipflop;  // 1 or 2
        flipflop = 3 - flipflop;  // Toggles: 1→2→1→2...
    }
    break;

// Return the created structure
return pred_dir;
```

**Memory state after `bpred_dir_create()`:**

```
pred_dir (at address, say 0x7fff1000) = {
    .class = BPred2bit,
    .config = {
        .bimod = {
            .size = 4096,
            .table = 0x7fff2000  ← Points to array of 4096 bytes
        }
    }
}

Memory at 0x7fff2000 (the table):
table[0] = 1    // Weakly not-taken
table[1] = 2    // Weakly taken
table[2] = 1    // Weakly not-taken
table[3] = 2    // Weakly taken
...
table[4095] = 2
```

### Step 0.3: Back in `bpred_create()` - Allocate BTB and RAS

```c
// Now pred->dirpred.bimod points to the direction predictor
pred->dirpred.bimod = 0x7fff1000;  // The returned pred_dir

// Allocate BTB
pred->btb.sets = 2048;
pred->btb.assoc = 1;
pred->btb.btb_data = calloc(2048 * 1, sizeof(struct bpred_btb_ent_t));

// Allocate RAS
pred->retstack.size = 8;
pred->retstack.tos = 7;  // Top-of-stack starts at size-1
pred->retstack.stack = calloc(8, sizeof(struct bpred_btb_ent_t));
```

**Final memory state after initialization:**

```
pred (at 0x7fff0000) = {
    .class = BPred2bit,
    .dirpred = {
        .bimod = 0x7fff1000,  ← Points to direction predictor
        .twolev = NULL,
        .meta = NULL
    },
    .btb = {
        .sets = 2048,
        .assoc = 1,
        .btb_data = 0x7fff3000  ← Points to BTB array
    },
    .retstack = {
        .size = 8,
        .tos = 7,
        .stack = 0x7fff4000  ← Points to RAS array
    },
    .lookups = 0,
    .addr_hits = 0,
    ...
}
```

---

## Phase 1: FIRST BRANCH ENCOUNTER (FETCH Stage)

### Scenario: First time seeing branch at 0x00401000

The processor is fetching instructions and encounters our branch for the first time.

### Step 1.1: `ruu_fetch()` calls `bpred_lookup()`

```c
// In ruu_fetch() at line ~4251
fetch_pred_PC = bpred_lookup(
    pred,                    // Our predictor (0x7fff0000)
    fetch_regs_PC,          // baddr = 0x00401000
    0,                      // btarget = 0 (not computed yet)
    op,                     // MD_BEQ (branch equal opcode)
    FALSE,                  // is_call = FALSE
    FALSE,                  // is_return = FALSE
    &(fetch_data[fetch_tail].dir_update),  // dir_update_ptr
    &stack_recover_idx      // For RAS recovery
);
```

### Step 1.2: Inside `bpred_lookup()` - Initial Setup

```c
md_addr_t bpred_lookup(struct bpred_t *pred,  // pred = 0x7fff0000
                       md_addr_t baddr,        // baddr = 0x00401000
                       md_addr_t btarget,      // btarget = 0
                       enum md_opcode op,      // op = MD_BEQ
                       int is_call,            // FALSE
                       int is_return,          // FALSE
                       struct bpred_update_t *dir_update_ptr,
                       int *stack_recover_idx)
{
    struct bpred_btb_ent_t *pbtb = NULL;
    int index, i;
    
    // Check if this is a control instruction
    if (!(MD_OP_FLAGS(op) & F_CTRL))
        return 0;  // Not a branch, return 0
    
    // It IS a branch, increment lookup counter
    pred->lookups++;  // Now = 1
    
    // Initialize the update structure
    dir_update_ptr->dir.ras = FALSE;
    dir_update_ptr->pdir1 = NULL;
    dir_update_ptr->pdir2 = NULL;
    dir_update_ptr->pmeta = NULL;
```

**Memory state of `dir_update_ptr` (at, say, 0x7ffff000):**

```
dir_update_ptr = {
    .pdir1 = NULL,
    .pdir2 = NULL,
    .pmeta = NULL,
    .dir = {
        .ras = 0,
        .bimod = 0,
        .twolev = 0,
        .meta = 0
    }
}
```

### Step 1.3: Direction Predictor Lookup

```c
    // Execute switch based on predictor class
    switch (pred->class) {
        case BPred2bit:
            // This is a conditional branch (not unconditional)
            if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND)) {
                // Call bpred_dir_lookup to get pointer to counter
                dir_update_ptr->pdir1 = bpred_dir_lookup(pred->dirpred.bimod, baddr);
                //                                       ↑                      ↑
                //                                  0x7fff1000              0x00401000
            }
            break;
    }
```

### Step 1.4: Inside `bpred_dir_lookup()`

```c
char *bpred_dir_lookup(struct bpred_dir_t *pred_dir,  // 0x7fff1000
                       md_addr_t baddr)                // 0x00401000
{
    unsigned char *p = NULL;
    
    switch (pred_dir->class) {
        case BPred2bit:
            // Compute hash index using BIMOD_HASH macro
            // BIMOD_HASH(PRED, ADDR) = 
            //     ((((ADDR) >> 19) ^ ((ADDR) >> MD_BR_SHIFT)) & (size - 1))
            
            // Let's compute step by step:
            // MD_BR_SHIFT is typically 2 (skip lower 2 bits)
            
            // baddr = 0x00401000 in binary
            // = 0000 0000 0100 0000 0001 0000 0000 0000
            
            // baddr >> 19 = 0x00000020
            // = 0000 0000 0000 0000 0000 0000 0010 0000
            
            // baddr >> 2 = 0x00100400
            // = 0000 0000 0001 0000 0000 0100 0000 0000
            
            // XOR them:
            // 0x00000020 ^ 0x00100400 = 0x00100420
            
            // Mask to table size (4096 - 1 = 0xFFF):
            // 0x00100420 & 0xFFF = 0x420
            // = 1056 in decimal
            
            int index = BIMOD_HASH(pred_dir, baddr);
            // index = 1056
            
            // Get pointer to counter at this index
            p = &pred_dir->config.bimod.table[index];
            //    ↑
            //  Address: 0x7fff2000 + 1056 = 0x7fff2420
            
            break;
    }
    
    return (char *)p;  // Return 0x7fff2420
}
```

**What's at that memory location?**

```
Memory at 0x7fff2420:
*p = table[1056] = 2  // Weakly taken (from initialization, even index)
```

### Step 1.5: Back in `bpred_lookup()` - Store the Pointer

```c
    // dir_update_ptr->pdir1 now points to the counter!
    dir_update_ptr->pdir1 = 0x7fff2420;  // Points to table[1056]
```

**Updated `dir_update_ptr` structure:**

```
dir_update_ptr = {
    .pdir1 = 0x7fff2420,  ← Now points to table[1056] which contains value 2
    .pdir2 = NULL,
    .pmeta = NULL,
    .dir = {
        .ras = 0,
        .bimod = 0,
        .twolev = 0,
        .meta = 0
    }
}
```

### Step 1.6: RAS Check (Return Address Stack)

```c
    // Save current RAS state for recovery
    if (pred->retstack.size)
        *stack_recover_idx = pred->retstack.tos;  // Save TOS = 7
    
    // Check if this is a return instruction
    if (is_return && pred->retstack.size) {
        // This is FALSE for our branch
    }
    
    // Check if this is a call instruction
    if (is_call && pred->retstack.size) {
        // This is FALSE for our branch
    }
```

### Step 1.7: BTB Lookup (Branch Target Buffer)

```c
    // Compute BTB index
    index = (baddr >> MD_BR_SHIFT) & (pred->btb.sets - 1);
    // index = (0x00401000 >> 2) & (2048 - 1)
    // index = 0x00100400 & 0x7FF
    // index = 0x400 = 1024
    
    if (pred->btb.assoc > 1) {
        // Multi-way associative (not our case)
    } else {
        // Direct-mapped BTB
        pbtb = &pred->btb.btb_data[index];  // Get entry at index 1024
        
        // Check if BTB entry matches our branch address
        if (pbtb->addr != baddr) {
            pbtb = NULL;  // BTB MISS (first time seeing this branch)
        }
    }
    
    // pbtb = NULL (BTB miss)
```

### Step 1.8: Generate Prediction

```c
    // Check if this is unconditional branch
    if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) == (F_CTRL | F_UNCOND)) {
        // Not unconditional, skip this
    }
    
    // This is a conditional branch
    if (pbtb == NULL) {
        // BTB MISS - return direction only (no target address)
        
        // Read the counter value through the pointer we saved
        // *dir_update_ptr->pdir1 = *(0x7fff2420) = 2
        
        if (*(dir_update_ptr->pdir1) >= 2) {
            return 1;  // Predicted TAKEN, but no target (BTB miss)
        } else {
            return 0;  // Predicted NOT TAKEN
        }
        
        // Since our counter = 2, we predict TAKEN
        return 1;
    }
}  // End of bpred_lookup()
```

**Return value: 1** (Taken, but target unknown)

### Step 1.9: Back in `ruu_fetch()`

```c
fetch_pred_PC = 1;  // Returned value

// Since return value is 1 (taken but no target), the fetch stage
// will continue with next sequential instruction for now
// The real target will be computed when the branch executes
```

**Summary of Phase 1:**
- **Counter accessed:** `table[1056]` at address `0x7fff2420`
- **Counter value:** `2` (Weakly Taken)
- **Prediction:** TAKEN (but BTB miss, so no target)
- **Pointer saved:** `dir_update_ptr->pdir1 = 0x7fff2420`

---

## Phase 2: BRANCH EXECUTION (Execute Stage)

The branch instruction executes and we discover the actual outcome.

### Step 2.1: Branch Resolves

```c
// In execution, the branch condition is evaluated
if (reg[1] == reg[2]) {
    actual_taken = TRUE;
    actual_target = 0x00401100;  // Target address
} else {
    actual_taken = FALSE;
    actual_target = 0x00401004;  // Fall-through (PC + 4)
}

// Let's say the branch WAS TAKEN
actual_taken = TRUE;
actual_target = 0x00401100;

// Compare with prediction
pred_taken = (fetch_pred_PC == 1);  // TRUE (we predicted taken)
correct = FALSE;  // BTB miss, so address prediction wrong
```

---

## Phase 3: UPDATE (Commit Stage)

Now we update the predictor with the actual outcome.

### Step 3.1: `ruu_commit()` calls `bpred_update()`

```c
// In ruu_commit() at line ~2219
bpred_update(pred,                  // 0x7fff0000
             rs->PC,                // 0x00401000 (branch address)
             rs->next_PC,           // 0x00401100 (actual target)
             taken,                 // TRUE
             pred_taken,            // TRUE
             correct,               // FALSE (BTB miss)
             rs->op,                // MD_BEQ
             &rs->dir_update);      // The dir_update_ptr we saved!
```

### Step 3.2: Inside `bpred_update()` - Initial Checks

```c
void bpred_update(struct bpred_t *pred,        // 0x7fff0000
                  md_addr_t baddr,             // 0x00401000
                  md_addr_t btarget,           // 0x00401100
                  int taken,                   // TRUE (1)
                  int pred_taken,              // TRUE (1)
                  int correct,                 // FALSE (0)
                  enum md_opcode op,           // MD_BEQ
                  struct bpred_update_t *dir_update_ptr)
{
    // Check if this is a control instruction
    if (!(MD_OP_FLAGS(op) & F_CTRL))
        return;  // It is, so continue
    
    // Update statistics
    if (correct)
        pred->addr_hits++;  // Not executed (correct = FALSE)
    
    // Check direction prediction
    if (!!pred_taken == !!taken)  // Both TRUE, so equal
        pred->dir_hits++;  // Correct direction! Now = 1
    else
        pred->misses++;
```

**Statistics after this update:**
```
pred->lookups = 1
pred->addr_hits = 0  (BTB miss)
pred->dir_hits = 1   (Direction correct)
pred->misses = 0
```

### Step 3.3: Update Direction Counter

```c
    // Check if we need to update direction predictor
    if (dir_update_ptr->pdir1) {
        // We DO have a pointer! It's 0x7fff2420
        
        if (taken) {  // TRUE
            // Branch was taken, increment counter (max 3)
            if (*dir_update_ptr->pdir1 < 3) {
                ++*dir_update_ptr->pdir1;
                
                // Current value: *(0x7fff2420) = 2
                // After increment: *(0x7fff2420) = 3
            }
        } else {
            // Branch was not taken (not executed)
        }
    }
```

**Memory after counter update:**

```
Memory at 0x7fff2420:
table[1056] = 3  ← Changed from 2 to 3 (now Strongly Taken)
```

### Step 3.4: Update BTB (Branch Target Buffer)

```c
    // Find BTB entry (only if taken)
    if (taken) {
        index = (baddr >> MD_BR_SHIFT) & (pred->btb.sets - 1);
        // index = 1024 (same as in lookup)
        
        pbtb = &pred->btb.btb_data[index];
        
        // Update BTB entry
        if (pbtb->addr == baddr) {
            // Entry already exists, update target if needed
            if (!correct)
                pbtb->target = btarget;
        } else {
            // Allocate new BTB entry
            pbtb->addr = baddr;          // 0x00401000
            pbtb->op = op;               // MD_BEQ
            pbtb->target = btarget;      // 0x00401100
        }
    }
}  // End of bpred_update()
```

**BTB after update:**

```
pred->btb.btb_data[1024] = {
    .addr = 0x00401000,     ← Branch address
    .op = MD_BEQ,           ← Opcode
    .target = 0x00401100,   ← Target address
    .prev = NULL,
    .next = NULL
}
```

**Summary of Phase 3:**
- **Counter updated:** `table[1056]` changed from `2` → `3`
- **BTB updated:** Entry at index `1024` now has target `0x00401100`
- **Statistics:** `dir_hits = 1`, `addr_hits = 0`

---

## Phase 4: SECOND ENCOUNTER (Same Branch Again)

Now the program loops back and we see the same branch again.

### Step 4.1: `bpred_lookup()` called again

```c
bpred_lookup(pred, 0x00401000, 0, MD_BEQ, FALSE, FALSE, 
             &new_dir_update, &stack_recover_idx);
```

### Step 4.2: Direction Predictor Lookup (Same as Before)

```c
// Compute index (same as before)
index = BIMOD_HASH(pred_dir, 0x00401000) = 1056

// Get pointer to counter
dir_update_ptr->pdir1 = &pred->dirpred.bimod->config.bimod.table[1056];
// = 0x7fff2420

// Read counter value
*dir_update_ptr->pdir1 = 3  ← NOW IT'S 3 (Strongly Taken)
```

### Step 4.3: BTB Lookup (Now with Entry)

```c
// Compute BTB index
index = (0x00401000 >> 2) & (2048 - 1) = 1024

// Get BTB entry
pbtb = &pred->btb.btb_data[1024];

// Check if address matches
if (pbtb->addr == baddr) {  // 0x00401000 == 0x00401000 → TRUE
    // BTB HIT!
    // pbtb points to valid entry with target = 0x00401100
}
```

### Step 4.4: Generate Prediction

```c
// Conditional branch with BTB hit
if (pbtb == NULL) {
    // Not executed
} else {
    // BTB HIT case
    if (*(dir_update_ptr->pdir1) >= 2) {  // 3 >= 2 → TRUE
        return pbtb->target;  // Return 0x00401100
    } else {
        return 0;  // Predicted not taken
    }
}

// Returns: 0x00401100 (Predicted TAKEN with correct target)
```

### Step 4.5: Back in `ruu_fetch()`

```c
fetch_pred_PC = 0x00401100;  // Got actual target address!

// Fetch stage now knows to fetch from 0x00401100
// Prediction is complete and accurate!
```

**Summary of Phase 4:**
- **Counter accessed:** `table[1056]` = `3` (Strongly Taken)
- **BTB status:** HIT with target `0x00401100`
- **Prediction:** TAKEN to address `0x00401100` ✓
- **Result:** Perfect prediction!

---

## Visual Summary: Pointer Chain

```
┌─────────────────────────────────────────────────────────────┐
│                    INITIALIZATION                           │
└─────────────────────────────────────────────────────────────┘

pred (0x7fff0000)
  │
  ├─> dirpred.bimod (0x7fff1000)
  │     │
  │     └─> config.bimod.table (0x7fff2000)
  │           │
  │           ├─> [0] = 1
  │           ├─> [1] = 2
  │           ├─> ...
  │           ├─> [1056] = 2  ← Our counter
  │           ├─> ...
  │           └─> [4095] = 2
  │
  ├─> btb.btb_data (0x7fff3000)
  │     │
  │     ├─> [0] = {empty}
  │     ├─> ...
  │     ├─> [1024] = {empty}  ← Our BTB entry
  │     ├─> ...
  │     └─> [2047] = {empty}
  │
  └─> retstack.stack (0x7fff4000)
        └─> [0..7] = {empty}

┌─────────────────────────────────────────────────────────────┐
│                    LOOKUP PHASE                             │
└─────────────────────────────────────────────────────────────┘

1. baddr = 0x00401000 enters bpred_lookup()
2. Compute hash: index = 1056
3. Get pointer: pdir1 = &table[1056] = 0x7fff2420
4. Read value: *pdir1 = 2
5. BTB lookup: index = 1024, MISS (first time)
6. Prediction: 2 >= 2 → TAKEN, but no target → return 1

dir_update_ptr (0x7ffff000)
  │
  └─> pdir1 (0x7fff2420) ──────┐
                               │
                               ▼
        table[1056] = 2   ← Points here!

┌─────────────────────────────────────────────────────────────┐
│                    UPDATE PHASE                             │
└─────────────────────────────────────────────────────────────┘

1. Branch executes: taken = TRUE, target = 0x00401100
2. bpred_update() called with same dir_update_ptr
3. Use saved pointer: pdir1 = 0x7fff2420
4. Update counter: *pdir1 = 2 → 3
5. Update BTB: btb_data[1024] = {addr=0x00401000, target=0x00401100}

AFTER UPDATE:

pred->dirpred.bimod->config.bimod.table[1056] = 3  ✓
pred->btb.btb_data[1024] = {
    .addr = 0x00401000,
    .target = 0x00401100
}  ✓

┌─────────────────────────────────────────────────────────────┐
│                    SECOND LOOKUP                            │
└─────────────────────────────────────────────────────────────┘

1. Same baddr = 0x00401000
2. Same index = 1056
3. Same pointer: pdir1 = 0x7fff2420
4. Read NEW value: *pdir1 = 3
5. BTB lookup: HIT! target = 0x00401100
6. Prediction: 3 >= 2 → TAKEN to 0x00401100  ✓ ✓
```

---

## Key Insights: Why Pointers Matter

### The Magic of `dir_update_ptr->pdir1`

```c
// During LOOKUP (FETCH stage):
dir_update_ptr->pdir1 = &table[1056];  // Save ADDRESS of counter

// Later during UPDATE (COMMIT stage, many cycles later):
++*dir_update_ptr->pdir1;  // Update SAME counter using saved address
```

**Why this works:**

1. **Lookup happens in FETCH** - very early in pipeline
2. **Update happens in COMMIT** - many cycles later
3. **The pointer bridges the gap!** - saves the exact memory location
4. **No need to recompute** - just use the saved pointer

### Counter State Machine in Action

```
First encounter:
  table[1056] = 2 (Weakly Taken)
              ↓ Branch taken, increment
  table[1056] = 3 (Strongly Taken)
  
Second encounter:
  table[1056] = 3 (Strongly Taken)
              ↓ Branch taken, no change (already at max)
  table[1056] = 3 (Strongly Taken)
  
If branch NOT taken:
  table[1056] = 3 (Strongly Taken)
              ↓ Branch not taken, decrement
  table[1056] = 2 (Weakly Taken)
```

---

## Common Confusions Clarified

### Q1: Why not just store the index instead of pointer?

**A:** Storing pointer is more flexible:
- Works with any predictor structure
- Avoids recomputing hash
- Handles complex index calculations once
- Pointer directly accesses memory

### Q2: What if the table is reallocated?

**A:** It's not. The table is allocated once at initialization and never moved.

### Q3: How does the pointer survive across pipeline stages?

**A:** The `dir_update_ptr` structure is stored in the instruction's entry in the RUU (Re-order buffer). It travels with the instruction through the pipeline.

### Q4: Why is the BTB separate from direction prediction?

**A:** 
- **Direction:** Taken or Not Taken (1 bit decision)
- **BTB:** WHERE to go if taken (32-bit address)
- Separating them allows BTB misses while direction is still predicted

---

## Memory Layout Summary

```
High Memory
  │
  ├─ 0x7ffff000: dir_update_ptr (temporary, on stack)
  │                └─ pdir1 = 0x7fff2420
  │
  ├─ 0x7fff4000: RAS (8 entries × 16 bytes = 128 bytes)
  │
  ├─ 0x7fff3000: BTB (2048 entries × 32 bytes = 64KB)
  │                └─ [1024] = our branch entry
  │
  ├─ 0x7fff2000: Bimodal table (4096 bytes)
  │                └─ [1056] = our counter (1 byte)
  │                             ↑
  │                             └─── pdir1 points here
  │
  ├─ 0x7fff1000: pred_dir structure (24 bytes)
  │                └─ Contains pointer to table
  │
  └─ 0x7fff0000: pred structure (128 bytes)
                   └─ Contains pointers to everything

Low Memory
```

---

## Complete Code Flow Timeline

```
Cycle 0: Initialization
  ├─ bpred_create(BPred2bit, 4096, ...)
  └─ bpred_dir_create(BPred2bit, 4096, ...)
      └─ Allocate table[4096], initialize to 1,2,1,2,...

Cycle 100: First fetch of branch at 0x00401000
  ├─ ruu_fetch()
  └─ bpred_lookup(pred, 0x00401000, ...)
      ├─ bpred_dir_lookup(pred->dirpred.bimod, 0x00401000)
      │   └─ Return &table[1056] (value = 2)
      ├─ Save: dir_update_ptr->pdir1 = &table[1056]
      ├─ Check BTB: MISS
      └─ Return: 1 (taken, no target)

Cycle 105: Branch decodes
  └─ ruu_dispatch()
      └─ Store dir_update in RUU entry

Cycle 110: Branch executes
  └─ Actual outcome: taken to 0x00401100

Cycle 115: Branch commits
  ├─ ruu_commit()
  └─ bpred_update(pred, 0x00401000, 0x00401100, taken=1, ...)
      ├─ Use saved pointer: dir_update_ptr->pdir1
      ├─ Update: *pdir1 = 2 → 3
      └─ Update BTB[1024] = {addr=0x00401000, target=0x00401100}

Cycle 200: Second fetch of same branch
  ├─ ruu_fetch()
  └─ bpred_lookup(pred, 0x00401000, ...)
      ├─ bpred_dir_lookup(...) → &table[1056] (value = 3)
      ├─ Check BTB: HIT! target = 0x00401100
      └─ Return: 0x00401100 (taken to target) ✓✓✓
```

---

## Try It Yourself: Debug Exercise

Add these debug prints to see it in action:

```c
// In bpred_dir_lookup() for BPred2bit:
case BPred2bit:
    int index = BIMOD_HASH(pred_dir, baddr);
    p = &pred_dir->config.bimod.table[index];
    
    fprintf(stderr, "[LOOKUP] addr=0x%08x, index=%d, counter=%d, ptr=%p\n",
            baddr, index, *p, p);
    
    break;

// In bpred_update():
if (dir_update_ptr->pdir1) {
    unsigned char old_val = *dir_update_ptr->pdir1;
    
    if (taken) {
        if (*dir_update_ptr->pdir1 < 3)
            ++*dir_update_ptr->pdir1;
    } else {
        if (*dir_update_ptr->pdir1 > 0)
            --*dir_update_ptr->pdir1;
    }
    
    fprintf(stderr, "[UPDATE] addr=0x%08x, taken=%d, counter=%d->%d, ptr=%p\n",
            baddr, taken, old_val, *dir_update_ptr->pdir1, 
            dir_update_ptr->pdir1);
}
```

You'll see output like:
```
[LOOKUP] addr=0x00401000, index=1056, counter=2, ptr=0x7fff2420
[UPDATE] addr=0x00401000, taken=1, counter=2->3, ptr=0x7fff2420
[LOOKUP] addr=0x00401000, index=1056, counter=3, ptr=0x7fff2420
[UPDATE] addr=0x00401000, taken=1, counter=3->3, ptr=0x7fff2420
```

Notice how the **pointer address stays the same**!

---

I hope this step-by-step walkthrough clarifies how the structures and pointers work together! The key is understanding that the pointer saved during lookup is used during update to access the exact same counter.
