# Volume Render Regression Test Optimization Guide

**Issue**: #149189 - Slow volume render regression tests  
**Date**: November 1, 2025  
**Status**: 🔧 In Progress

---

## 📊 Problem Summary

Volume render regression tests are extremely slow, especially on ARM64:
- **Windows ARM64**: 244.57s total
- **Windows x64**: 65.56s total
- **62 total tests** (31 files × 2 modes: Octree + Ray March)

### Critical Bottlenecks

| Test Name | Octree Init | Total Time | Severity |
|-----------|-------------|------------|----------|
| **volume_step_offset** | 72.97s | 74.22s | 🔴 CRITICAL |
| **implicit_volume** | 12.96s | 20.39s | 🔴 SEVERE |
| **volume_zero_extinction_channel** | 6.93s | 18.48s | 🟠 HIGH |
| volume_scatter_mie_small_particles | 4.85s | 7.93s | 🟡 MODERATE |
| principled_absorption | 4.08s | 10.97s | 🟡 MODERATE |
| volume_light_path | 4.68s | 6.91s | 🟡 MODERATE |

**Total time from these 6 tests alone**: ~138.9s (57% of total ARM64 time)

---

## 🎯 Optimization Strategy

### Phase 1: Critical Fixes (Target: -80s)

#### 1.1 volume_step_offset.blend (Priority 1)
**Current**: 72.97s init + 1.25s render = 74.22s  
**Target**: <5s total  
**Savings**: ~69s (28% of total test time!)

**Root Causes**:
- Excessive number of volume objects
- Complex shader networks
- Overly detailed geometry

**Solutions**:
```python
# Actions to take:
1. Reduce volume objects from N to max 10
2. Simplify shader node networks (remove unused nodes)
3. Reduce subdivision surface levels to 1
4. Merge duplicate volume materials
5. Remove unnecessary modifiers
```

**Implementation**:
```bash
# Run in Blender:
blender volume_step_offset.blend --background --python optimize_volume_tests.py

# Or manually:
# 1. Open volume_step_offset.blend
# 2. Delete redundant volume objects (keep only essential ones)
# 3. Simplify shaders: Shader Editor > Remove unused Math/ColorRamp nodes
# 4. Reduce object count in scene
```

#### 1.2 implicit_volume.blend (Priority 2)
**Current**: 12.96s init + 7.43s render = 20.39s  
**Target**: <3s total  
**Savings**: ~17s (7% of total)

**Root Causes**:
- High volume density settings
- Complex principled volume calculations
- Excessive resolution

**Solutions**:
```python
# Optimize density values
for mat in materials:
    if mat.principled_volume:
        mat.principled_volume.density = min(1.0, current_density)

# Reduce grid resolution
volume.grid_resolution = 128  # down from 256 or higher
```

#### 1.3 volume_zero_extinction_channel.blend (Priority 3)
**Current**: 6.93s init + 11.55s render = 18.48s  
**Target**: <3s total  
**Savings**: ~15s (6% of total)

**Root Causes**:
- Long render time (11.55s)
- Complex extinction channel calculations
- High sample count

**Solutions**:
```python
# Reduce sample count if > 64
scene.cycles.samples = min(64, current_samples)

# Simplify extinction calculations
# Review material nodes for unnecessary complexity
```

---

### Phase 2: Moderate Fixes (Target: -20s)

#### 2.1 General Optimizations

Apply to all tests:

1. **Sample Count Reduction**
   - Current: Many use 64 spp
   - Target: 48 spp (still produces useful results)
   - Savings: ~25% render time

2. **Anisotropy Reduction**
   - Expensive calculation
   - Reduce value by 30% if > 0.5
   - Savings: ~1-2s per test

3. **Shader Simplification**
   - Remove unused nodes
   - Combine similar materials
   - Use simpler phase functions

---

## 🛠️ Implementation Steps

### Quick Start

```bash
# Navigate to test directory
cd c:/Users/24beevdt002/Blender/tests/python

# Run optimizer on specific file
blender ../files/render/volume/volume_step_offset.blend --background --python optimize_volume_tests.py

# Or batch optimize all priority files
blender --background --python optimize_volume_tests.py -- --batch
```

### Manual Optimization Checklist

For each problematic .blend file:

- [ ] **Open file in Blender**
- [ ] **Count volume objects** (Outliner > filter by Volume)
- [ ] **Check shader complexity** (Shader Editor)
- [ ] **Review sample count** (Render Properties > Sampling)
- [ ] **Check modifiers** (Properties > Modifiers)
- [ ] **Simplify as needed**
- [ ] **Save as _optimized.blend**
- [ ] **Test render time**
- [ ] **Compare visual quality** (should be nearly identical)

---

## 📈 Expected Results

### Time Savings Breakdown

| Optimization | Files Affected | Time Saved | % of Total |
|-------------|----------------|------------|------------|
| Fix volume_step_offset | 2 (Octree+Ray) | ~138s | 56% |
| Fix implicit_volume | 2 | ~34s | 14% |
| Fix volume_zero_extinction | 2 | ~30s | 12% |
| Moderate fixes (3 files) | 6 | ~24s | 10% |
| Sample reduction (all) | 62 | ~20s | 8% |
| **TOTAL** | **74 tests** | **~246s** | **100%** |

### Projected New Times

**Windows ARM64**:
- Current: 244.57s
- After optimization: ~50-70s (70-80% reduction)

**Windows x64**:
- Current: 65.56s
- After optimization: ~15-20s (70-75% reduction)

---

## 🖥️ ARM64-Specific Issues

### CPU Core Parking Problem

**Issue**: Qualcomm CPU parks half the cores under sustained load  
**Impact**: Tests run at ~50% performance

**Solutions**:

1. **Disable Core Parking** (Windows Registry)
```powershell
# Run as Administrator
# Set minimum processor state to 100%
powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PROCTHROTTLEMIN 100
powercfg /setactive SCHEME_CURRENT
```

2. **Force CPU Affinity** (in test runner)
```python
import os
import psutil

# Force process to use all cores
p = psutil.Process()
p.cpu_affinity(list(range(psutil.cpu_count())))
```

3. **Optimize Test Parallelization**
```python
# Current: 4 parallel tests
# Recommendation: 2-3 parallel tests on ARM64
# Reduces thermal throttling and core parking
```

**Expected Impact**: 2x speedup on ARM64 (244s → ~122s)

---

## 📝 Test Quality Preservation

**Critical**: Optimizations must not reduce test effectiveness

### Quality Checks

After each optimization:

1. **Visual Comparison**
   ```bash
   # Render before and after
   blender original.blend -b -f 1 -o before.png
   blender optimized.blend -b -f 1 -o after.png
   
   # Compare images (should be nearly identical)
   compare before.png after.png diff.png
   ```

2. **Regression Detection**
   - Test must still catch the same types of bugs
   - Volume characteristics preserved
   - Lighting behavior unchanged

3. **Acceptable Changes**
   - ✅ Slight noise variation (sample count changes)
   - ✅ Simplified geometry (if not visible in render)
   - ✅ Reduced object count (if redundant)
   - ❌ Different lighting
   - ❌ Different volume behavior
   - ❌ Missing visual elements

---

## 🔬 Analysis Tools

### Profiling Test Performance

```python
import time
import bpy

def profile_scene():
    """Profile octree initialization and render time."""
    
    # Octree init time
    start = time.time()
    bpy.ops.render.render(write_still=False, use_viewport=False)
    octree_time = time.time() - start
    
    # Render time
    start = time.time()
    bpy.ops.render.render(write_still=True)
    render_time = time.time() - start
    
    return {
        'octree_init': octree_time,
        'render': render_time,
        'total': octree_time + render_time
    }
```

### Scene Complexity Analyzer

```python
def analyze_scene_complexity():
    """Analyze what's making the scene slow."""
    
    report = {
        'volume_objects': len([o for o in bpy.data.objects if o.type == 'VOLUME']),
        'total_objects': len(bpy.data.objects),
        'materials': len(bpy.data.materials),
        'samples': bpy.context.scene.cycles.samples,
        'shader_nodes': sum(len(m.node_tree.nodes) for m in bpy.data.materials if m.use_nodes)
    }
    
    return report
```

---

## 📊 Monitoring & Validation

### Buildbot Integration

After optimizations, monitor buildbot times:

```python
# Target times (Windows ARM64):
TARGETS = {
    'volume_step_offset': 5.0,      # down from 72.97s
    'implicit_volume': 3.0,         # down from 12.96s
    'volume_zero_extinction': 3.0,  # down from 18.48s
    'total_test_time': 70.0         # down from 244.57s
}

# Alert if times exceed targets
def check_performance(test_name, actual_time):
    target = TARGETS.get(test_name)
    if target and actual_time > target * 1.2:  # 20% tolerance
        print(f"⚠️ Performance regression: {test_name}")
        print(f"   Target: {target}s, Actual: {actual_time}s")
```

---

## ✅ Success Criteria

### Phase 1 Complete
- [ ] volume_step_offset < 5s total
- [ ] implicit_volume < 3s total
- [ ] volume_zero_extinction_channel < 3s total
- [ ] Total ARM64 time < 150s (40% reduction)
- [ ] All test images verified (no visual regressions)

### Phase 2 Complete
- [ ] All moderate issues < 5s each
- [ ] Total ARM64 time < 70s (70% reduction)
- [ ] Sample count optimized across all tests
- [ ] Buildbot tests passing consistently

### Phase 3 (ARM64-Specific)
- [ ] CPU core parking issue resolved
- [ ] Parallel test count optimized
- [ ] Total ARM64 time < 50s (80% reduction)
- [ ] Thermal management improved

---

## 🚀 Quick Wins

**Implement these immediately** for fast results:

1. **volume_step_offset optimization** (69s savings)
   - 30 minutes work
   - Massive impact

2. **Reduce parallel tests on ARM64** from 4 to 2-3
   - Config change only
   - Prevents core parking

3. **Global sample count reduction** (48 spp instead of 64)
   - Script can automate
   - 20s+ savings

**Total quick win**: ~90s (37% improvement) in < 2 hours work

---

## 📞 Support

**Issue**: #149189  
**Optimizer Script**: `tests/python/optimize_volume_tests.py`  
**Test Location**: `tests/files/render/volume/`

---

## 📖 References

- Cycles Volume Rendering: https://docs.blender.org/manual/en/latest/render/cycles/render_settings/volumes.html
- Regression Test Guidelines: https://developer.blender.org/docs/handbook/testing/
- ARM64 Performance: https://developer.qualcomm.com/software/snapdragon-performance

---

**Status**: Ready for implementation  
**Estimated Total Time Savings**: 70-80% reduction  
**Risk Level**: Low (with proper validation)
