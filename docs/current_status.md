# Current Status

## Work Completed

### Phase 1: Diagnosis
- Built minimal D3D11 hook DLL with Map/Unmap/Flush logging
- Captured 30MB+ logs during gameplay with 3 menu opens
- Analyzed log data with Python scripts

### Phase 1 Findings
- 317,471 Map operations total (98.7% WRITE_DISCARD, 1.3% READ)
- 4,010 Flush operations total
- Menu opens show 0 READ operations, all WRITE_DISCARD
- Flush calls ignored successfully (confirmed via logs)
- Flush filtering had no measurable impact on lag (still 1.5s after DLL removed)

### Phase 1 DXVK Metrics (from earlier testing)
During menu opens:
- Queue submissions: 1-5 baseline → 1000 during menu
- Queue syncs: 1-5 baseline → 1000 during menu
- Barriers: 10-50 baseline → 2400 during menu
- CS Syncs: 1-5 baseline → 1000 during menu

## Root Cause Analysis

### Initial Hypothesis (INCORRECT)
- Flush() calls causing GPU sync stalls
- Expected: Filtering 99% of flushes would eliminate lag

### Actual Result
- Flush filtering had zero impact
- Lag remains ~1.5s with or without DLL

### Current Hypothesis
Based on doitsujin's Sophie 2 fix (README.md):
- CopyResource() operations causing CPU-GPU synchronization
- Map() blocking while waiting for CopyResource to complete
- Pattern: CopyResource → Map (blocks) → Unmap → CopySubresourceRegion
- Each cycle forces full CPU-GPU sync
- Multiple sync points per menu open

### Evidence
- Sophie 2 had ~20 GPU sync points in main menu
- doitsujin's fix: shadow buffers to avoid GPU copies
- Arland DX likely has similar pattern but different resource parameters

### Cross-Platform Confirmation
- Issue occurs on Windows native (not DXVK-specific)
- Issue occurs on Switch port
- Pattern suggests engine-level problem from PS4 port (unified memory → discrete GPU)

### Why doitsujin's Fix Failed on Arland
- Error: 0x80070057 (E_INVALIDARG) repeated ~15,000 times
- Cause: Resource parameter mismatch between Sophie 2 and Arland DX
- doitsujin's fix creates shadow staging buffers with hardcoded parameters for Sophie 2
- Arland's older engine uses different resource descriptions
- Need to discover Arland's actual resource parameters to create compatible shadows

### Not Hooked Yet
- CopyResource
- CopySubresourceRegion
- UpdateSubresource
- Resource creation (CreateBuffer, CreateTexture2D)
- Present

### Not Tested
- RenderDoc frame capture during menu lag
- Shadow buffer approach (doitsujin's method)
- DXVK configuration tweaks

## Next Steps (Priority Order)

### Option 1: Hook CopyResource + Log Resource Parameters
Add hooks for:
- CopyResource (vtable index 47)
- CopySubresourceRegion (vtable index 46)
- UpdateSubresource (vtable index 48)

Log resource descriptions to find Arland's parameters:
- D3D11_BUFFER_DESC: ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags
- D3D11_TEXTURE2D_DESC: Width, Height, Format, Usage, BindFlags, CPUAccessFlags

Two sub-approaches:
- **A: Hook CopyResource** - Log descriptions of src/dst resources being copied
- **B: Hook CreateBuffer/CreateTexture2D** - See all resources created during menu opens

Goal: Compare Arland's parameters to Sophie 2's and adapt shadow buffer creation logic.

### Option 2: RenderDoc Capture
- Capture frame during menu open
- Identify exact D3D11 operation sequence
- Measure timing of individual operations
- Confirm CopyResource hypothesis

### Option 3: Implement Shadow Buffers
If CopyResource confirmed as bottleneck:
- Create CPU-side shadow copies of GPU resources
- Intercept CopyResource and copy from shadow instead
- Requires matching resource parameters (why doitsujin's fix failed on Arland)

### Option 4: Accept Defeat
If shadow buffers also fail:
- Document findings
- Consider DXVK-level fix
- Or accept game is unfixable

## Key Questions

1. Are CopyResource operations happening during menu opens?
2. If yes, how many and what resource types (buffers vs textures)?
3. What are Arland's resource parameters (Usage, CPUAccessFlags, BindFlags)?
4. How do Arland's parameters differ from Sophie 2's?
5. Can we modify doitsujin's shadow buffer logic to work with Arland's parameters?

## References
- doitsujin atelier-sync-fix: https://github.com/doitsujin/atelier-sync-fix
- Sophie 2 had CopyResource → Map(READ_WRITE) pattern causing sync
- Arland may have similar but with different resource formats/usage flags
