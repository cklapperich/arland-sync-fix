# Atelier Meruru DX Menu Lag - Phase 1 Findings

**Date:** October 23, 2025  
**Platform:** Pop!_OS Linux, Steam Proton DXVK v2.7.1, RTX 2080  
**Method:** DXVK HUD metrics

---

## Observations

**Launch:** `DXVK_LOG_LEVEL=info DXVK_HUD=full %command%`

| Metric | Baseline | Menu Open | Change |
|--------|----------|-----------|--------|
| Queue submissions | 1-5 | 1000 | 200x |
| Queue syncs | 1-5 | 1000 | 200x |
| Barriers | 10-50 | 2400 | 48-240x |
| CS Syncs | 1-5 | 1000 | 200x |
| Descriptor usage | 11KB | 87KB | 8x |
| Draw calls | ~50-100 | ~50-100 | None |
| Render passes | ~5-10 | ~5-10 | None |

**Timeline:**
1. Menu open button pressed
2. GPU spike: 1000 submissions, 2400 barriers
3. 2-second freeze
4. Menu appears
5. Metrics return to baseline
6. Menu closed → spike repeats

**Tested:** Cauldron, Container, Main menu, Shop inventory. Pattern consistent.

---

## Root Cause

Game submits 1000 individual GPU commands during menu initialization with synchronous CPU-GPU waits. Estimated 1-2ms per command = 1-2 second stall.

Commands likely: resource creation/updates (textures, buffers, render targets) for UI elements.

Engine does not batch these operations.

---

## Fix Potential

If 1000 submissions batched to 10: 2000ms → 20ms (100x improvement).

---

## Next Steps (Priority Order)

### 1. Try DXVK tweaks (30 min)

```bash
DXVK_COMMAND_QUEUE_SIZE=16384 %command%
DXVK_PRESENT_WAIT=0 %command%
DXVK_BATCH_SUBMISSIONS=1 %command%
```

Test if spike reduces. If yes, partial fix achieved.

### 2. RenderDoc frame capture (2-4 hours)

Capture during menu open to identify:
- Command types (resource creation? copies? rendering?)
- Resource types and parameters
- Exact operation sequence

### 3. D3D11 hook with command batching (6-12 weeks)

Create wrapper DLL that:
- Intercepts D3D11 submissions
- Buffers commands
- Submits in batches (1000 → 10)

Similar approach to doitsujin's atelier-sync-fix but targeting queue batching.

---

## Unknowns

- Exact command types in the 1000
- Whether commands have dependencies (limits batching)
- Windows native behavior (only tested Proton)
- Whether fix works across all three Arland games

---

## Success Metrics

**MVP:** Menu lag 2.0s → <0.5s, works on Meruru DX, no crashes

**Target:** Menu lag 2.0s → <0.1s, works on all three games, both platforms

---

## Previous Attempts

doitsujin's atelier-sync-fix failed with error 0x80070057 (INVALIDARG). Fix designed for different resource types/parameters than Arland DX uses. Present findings show the actual problem is queue submission volume, not just resource copying.

---

## Timing Breakdown

Total Menu open time is 1840ms
Queue sync time: 350ms (17.5% of lag)
CS sync time: 43ms (2% of lag)
Unaccounted: ~1450ms (80% of lag - GPU execution)

The 2000ms stall is mostly GPU executing 1000 commands inefficiently, not CPU waiting. Batching improves GPU execution efficiency directly.