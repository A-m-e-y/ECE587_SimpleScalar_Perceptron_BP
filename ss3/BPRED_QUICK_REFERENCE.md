# Quick Reference: Branch Predictor Functions

## Function Call Sequence

```
INITIALIZATION:
1. bpred_create() → Creates main predictor structure
2. bpred_dir_create() → Creates direction predictor(s)
3. bpred_reg_stats() → Registers statistics

DURING SIMULATION (per branch):
4. bpred_lookup() → Predicts branch (FETCH stage)
   └→ bpred_dir_lookup() → Gets pointer to counter
5. [Branch executes]
6. bpred_update() → Updates predictor state (ID/WB/COMMIT stage)
7. [If misprediction] bpred_recover() → Restores RAS
```

---

## Function Summary

### 1. `bpred_create()`
**Purpose:** Allocate and initialize entire branch predictor  
**Called:** Once at simulator startup  
**Returns:** `struct bpred_t *` - Main predictor structure  

**What to implement:**
```c
case BPredNeil:
    // Create your direction predictor(s)
    pred->dirpred.bimod = bpred_dir_create(BPredNeil, size, ...);
    // BTB and RAS allocated automatically
    break;
```

---

### 2. `bpred_dir_create()`
**Purpose:** Create and initialize direction predictor tables  
**Called:** From `bpred_create()`  
**Returns:** `struct bpred_dir_t *` - Direction predictor structure  

**What to implement:**
```c
case BPredNeil:
    // Allocate your custom tables
    pred_dir->config.neil.custom_table = calloc(size, ...);
    // Initialize counters/weights
    for (i = 0; i < size; i++)
        pred_dir->config.neil.custom_table[i] = initial_value;
    break;
```

---

### 3. `bpred_dir_lookup()`
**Purpose:** Get pointer to predictor state for this branch  
**Called:** From `bpred_lookup()` during prediction  
**Returns:** `char *` - Pointer to counter/weight for this branch  

**What to implement:**
```c
case BPredNeil:
    // Compute index from address (and history if needed)
    index = your_hash_function(baddr, history);
    index = index & (table_size - 1);
    // Return pointer to predictor state
    p = &pred_dir->config.neil.custom_table[index];
    break;
```

**KEY:** This pointer will be used later in `bpred_update()`!

---

### 4. `bpred_lookup()`
**Purpose:** Predict next PC for a branch  
**Called:** Every branch in FETCH stage  
**Returns:** 
- `0` = not taken
- `1` = taken (BTB miss, target unknown)
- `target_address` = taken (BTB hit, target known)

**What to implement:**
```c
case BPredNeil:
    if ((MD_OP_FLAGS(op) & (F_CTRL | F_UNCOND)) != (F_CTRL | F_UNCOND)) {
        // For conditional branches
        dir_update_ptr->pdir1 = bpred_dir_lookup(pred->dirpred.bimod, baddr);
        // Store any additional state for update
    }
    break;
```

**Common code handles:**
- Return address stack (RAS) for calls/returns
- Branch target buffer (BTB) lookup
- Final prediction generation

---

### 5. `bpred_update()`
**Purpose:** Update predictor state after branch resolves  
**Called:** In ID, WB, or COMMIT stage (depending on config)  
**Parameters:**
- `taken` = actual outcome (1=taken, 0=not-taken)
- `pred_taken` = what you predicted
- `correct` = was prediction correct?
- `dir_update_ptr` = state from lookup (contains pointers)

**What to implement:**
```c
// Update history registers (if any)
if (pred->class == BPredNeil) {
    // Update your global/local history
    pred->dirpred.bimod->config.neil.global_history = 
        (pred->dirpred.bimod->config.neil.global_history << 1) | taken;
    // Mask to history length
    pred->dirpred.bimod->config.neil.global_history &= history_mask;
}

// Update prediction counters/weights
if (dir_update_ptr->pdir1) {
    // Standard 2-bit saturating counter:
    if (taken) {
        if (*dir_update_ptr->pdir1 < 3)
            ++*dir_update_ptr->pdir1;
    } else {
        if (*dir_update_ptr->pdir1 > 0)
            --*dir_update_ptr->pdir1;
    }
    
    // OR implement your own update logic (e.g., perceptron weight update)
}
```

**Common code handles:**
- Statistics update
- BTB update (for taken branches)

---

### 6. `bpred_recover()`
**Purpose:** Restore RAS after misprediction  
**Called:** When misprediction detected  

**Usually don't need to modify** - just restores return address stack

---

### 7. `bpred_config()` and `bpred_dir_config()`
**Purpose:** Print configuration information  
**Called:** At startup  

**What to implement:**
```c
case BPredNeil:
    fprintf(stream, "pred_dir: neil: description\n");
    fprintf(stream, "  param1: %d\n", value1);
    break;
```

---

## Data Flow Diagram

```
bpred_lookup() [FETCH]
    |
    | Stores pointer in dir_update_ptr->pdir1
    |
    v
[Branch prediction made: 0, 1, or target_address]
    |
    | Branch executes...
    |
    v
bpred_update() [ID/WB/COMMIT]
    |
    | Uses pointer from dir_update_ptr->pdir1
    | to update the same counter
    v
[Predictor state updated]
```

---

## 2-Bit Saturating Counter Cheat Sheet

```
Counter Value | Prediction | After Taken | After Not-Taken
--------------|------------|-------------|----------------
      0       | Not Taken  |      1      |       0
      1       | Not Taken  |      2      |       0
      2       | Taken      |      3      |       1
      3       | Taken      |      3      |       2

Prediction rule: counter >= 2 ? TAKEN : NOT_TAKEN
```

---

## Common Hash Functions

```c
// Simple direct-mapped
index = (baddr >> MD_BR_SHIFT) & (size - 1);

// XOR folding
index = ((baddr >> 19) ^ (baddr >> MD_BR_SHIFT)) & (size - 1);

// With global history (Gshare)
index = ((baddr >> MD_BR_SHIFT) ^ global_history) & (size - 1);

// With local history (PAg)
l1_index = (baddr >> MD_BR_SHIFT) & (l1_size - 1);
local_history = history_table[l1_index];
index = local_history & (l2_size - 1);

// Combined (PAp)
l1_index = (baddr >> MD_BR_SHIFT) & (l1_size - 1);
local_history = history_table[l1_index];
index = (local_history | ((baddr >> MD_BR_SHIFT) << shift_width)) & (l2_size - 1);
```

---

## Testing Your Predictor

### 1. Compile
```bash
cd /home/ameyk/windows_d_drive/PSU/Adv_Comp_Arch_Yuchen/Project/simulator/ss3
make clean
make sim-outorder
```

### 2. Run with BPredNeil
```bash
./sim-outorder -bpred neil \
  -bpred:neil 4096 1 1 1 12 0 2048 1 8 \
  tests-pisa/bin.little/test-math
```

**Parameters for `-bpred:neil`:**
1. `4096` - bimod_size (use for your main table)
2. `1` - l1size (optional, use if needed)
3. `1` - l2size (optional, use if needed)
4. `1` - meta_size (optional, use if needed)
5. `12` - shift_width (history bits, if applicable)
6. `0` - xor (XOR flag, if applicable)
7. `2048` - BTB sets
8. `1` - BTB associativity
9. `8` - RAS size

### 3. Check Output
Look for lines like:
```
bpred_neil.lookups         # Total branches seen
bpred_neil.addr_hits       # Correct target predictions
bpred_neil.dir_hits        # Correct direction predictions
bpred_neil.misses          # Mispredictions
bpred_neil.bpred_dir_rate  # Accuracy = dir_hits / (dir_hits + misses)
```

---

## Minimal Implementation Checklist

- [ ] Add `neil` struct to `bpred_dir_t` union in `bpred.h`
- [ ] Add `case BPredNeil:` in `bpred_create()` (line ~86)
- [ ] Add `case BPredNeil:` in `bpred_dir_create()` (line ~184)
- [ ] Add `case BPredNeil:` in `bpred_dir_lookup()` (line ~498)
- [ ] Add `case BPredNeil:` in `bpred_lookup()` (line ~587)
- [ ] Add history update for BPredNeil in `bpred_update()` (line ~835)
- [ ] Add `case BPredNeil:` in `bpred_config()` (line ~310)
- [ ] Add `case BPredNeil:` in `bpred_dir_config()` (line ~272)
- [ ] Add `case BPredNeil:` in allocate ret-addr stack switch (line ~125)
- [ ] Test and debug!

---

## Common Pitfalls

1. **Forgetting to add BPredNeil to all switch statements**
   - Will cause fall-through or panic
   - Use grep to find all switch cases on pred->class

2. **Not preserving pointers between lookup and update**
   - MUST store pointer in dir_update_ptr->pdir1
   - Same pointer used in update

3. **Index out of bounds**
   - Always mask index: `index & (size - 1)`
   - Use power-of-2 sizes for fast masking

4. **Not initializing tables**
   - Calloc sets to 0 (strongly not-taken)
   - Better: initialize to 1 or 2 (weakly predicted)

5. **Updating wrong predictor state**
   - Make sure you're updating your BPredNeil tables
   - Not the BPredComb tables!

---

## Where to Find Examples

- **BPred2bit**: Simple bimodal predictor (lines 246-262 in bpred.c)
- **BPred2Level**: Two-level adaptive (lines 205-243 in bpred.c)
- **BPredComb**: Combined predictor (lines 86-100, 587-615 in bpred.c)

Study how they:
1. Define structures
2. Initialize tables
3. Compute indices
4. Update history
5. Update counters

---

## Good Luck!

You now have everything you need to implement BPredNeil. Start with the example in `BPRED_NEIL_EXAMPLE.c` (a simple Gshare predictor), get it working, then customize it with your own algorithm!

**Key principle:** The framework handles RAS, BTB, and statistics. You just need to:
1. Store your predictor state
2. Hash address to get index
3. Return pointer to state
4. Update state when branch resolves

That's it! The rest is handled for you.
