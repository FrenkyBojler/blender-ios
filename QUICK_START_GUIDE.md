# VR Director Viewfinder - Quick Start Guide

## 🎬 Overview
This guide will help you test the VR Director Viewfinder implementation.

---

## ✅ Pre-Flight Checklist

### 1. System Requirements
- [ ] Blender 3.2.0 or newer
- [ ] Built with OpenXR support (`xr_openxr`)
- [ ] OpenXR runtime installed:
  - SteamVR (recommended)
  - Oculus Runtime
  - Windows Mixed Reality
  - Other OpenXR-compatible runtime
- [ ] VR headset connected and powered on
- [ ] VR controllers paired and charged

### 2. Blender Setup
- [ ] "VR Scene Inspection" addon enabled
  - Edit > Preferences > Add-ons
  - Search for "VR Scene Inspection"
  - Check the enable box
- [ ] Scene camera created (for export testing)

---

## 🧪 Validation Test (No VR Required)

### Run the validation script:
1. Open Blender
2. Go to **Scripting** workspace
3. Click **Open** and navigate to:
   ```
   scripts/addons_core/viewport_vr_preview/test_viewfinder.py
   ```
4. Click **Run Script**
5. Check console output for validation results

**Expected Result**: "✅ ALL VALIDATIONS PASSED!"

---

## 🥽 VR Testing Workflow

### Phase 1: Start VR Session

1. Open **3D View** > **Sidebar** (press N)
2. Click **VR** tab
3. Click **"Start VR Session"**
4. Put on your VR headset
5. You should see your 3D scene in VR

**Troubleshooting:**
- If session doesn't start, check OpenXR runtime is running
- Verify headset is detected by runtime
- Check Blender console for error messages

---

### Phase 2: Enable Viewfinder

1. In the **VR** tab (visible in headset via desktop mirror)
2. Scroll down to **"Director Viewfinder"** panel
3. Click the **checkbox** in the panel header to enable
4. Look at your **left VR controller**
5. You should see a **white rectangular frame** (viewfinder)

**What to Look For:**
- White frame attached to left controller
- Frame follows controller movement smoothly
- 16:9 aspect ratio
- Slight transparency (alpha 0.8)

**Settings to Try:**
- Adjust **Size** slider → frame should resize
- Adjust **Passepartout** → overscan area changes

---

### Phase 3: Test Live Mode

#### 3A. Lens Adjustment
1. Ensure **Mode** is set to **"Live"**
2. Click **"Lens +"** button
   - Active camera lens should increase
   - Check camera properties in Properties panel
3. Click **"Lens -"** button
   - Lens value should decrease
4. Verify range limits (1mm - 5000mm)

#### 3B. Aperture Adjustment
1. Click **"Aperture +"** button
   - Should cycle through: f/1.4, f/2, f/2.8, f/4, f/5.6, f/8, f/11, f/16, f/22
2. Click **"Aperture -"** button
   - Should cycle backward
3. Check DOF settings in camera properties

#### 3C. Depth of Field Toggle
1. Click **"Toggle DOF"** button
   - Camera DOF should turn on
2. Click again
   - Camera DOF should turn off
3. Note: Aperture/Focus controls are disabled when DOF is off

#### 3D. Capture Shots
1. Hold **left controller** in a camera-like position
2. Frame your shot through the viewfinder
3. Click **"Capture Shot"** button (or use trigger in VR)
4. Check **Landmarks** panel
   - New landmark should appear: "Shot_001"
5. Move to different position
6. Capture another shot: "Shot_002"
7. Repeat 3-5 times for variety

**Expected Results:**
- Each capture creates a new VIEWFINDER landmark
- Landmarks are auto-named Shot_001, Shot_002, etc.
- Landmarks appear in the list
- Current lens/aperture/DOF settings are stored

---

### Phase 4: Test Playback Mode

#### 4A. Switch to Playback
1. Change **Mode** dropdown to **"Playback"**
2. UI should change to show playback controls
3. Viewfinder should still be visible

#### 4B. Navigate Shots
1. Click **"Next"** button (→ icon)
   - Should advance to next viewfinder shot
   - Landmark selection updates
2. Click **"Previous"** button (← icon)
   - Should go back to previous shot
3. Navigate through all captured shots

#### 4C. Preview Shots
1. Select a shot using Next/Previous
2. Click **"Preview Shot"** button (👁 icon)
3. Your VR viewpoint should **teleport** to that shot's position
4. You should see the scene from that camera angle
5. Try previewing different shots

#### 4D. Delete Shot
1. Navigate to a shot you want to delete
2. Click **"Delete Shot"** button (🗑 icon)
3. Shot should be removed from landmarks list
4. Selection should adjust to next available shot

---

### Phase 5: Export to Animation

#### 5A. Prepare Scene
1. Exit VR or work from desktop view
2. Ensure you have an **active camera**
   - If not, add one: Add > Camera
   - Make it active: Select camera > Ctrl+Numpad 0
3. Note camera's current position

#### 5B. Export Keyframes
1. In **VR** tab > **Director Viewfinder** panel
2. Click **"Export to Camera Keyframes"** button (🔑 icon)
3. Check console for confirmation message
4. Open **Timeline** or **Dope Sheet**

**Expected Results:**
- Camera has keyframes on location, rotation, and lens
- Keyframes are spaced 24 frames apart
- Number of keyframes = number of viewfinder shots
- Camera animates through all captured positions

#### 5C. Review Animation
1. Press **Spacebar** to play animation
2. Camera should move through all your captured shots
3. Check camera properties:
   - Lens changes per keyframe
   - DOF settings update (if enabled)
   - Aperture/Focus animate (if DOF was on)

---

## 🎯 Feature Checklist

Test each feature and mark as complete:

### Basic Features
- [ ] Viewfinder appears on left controller
- [ ] Viewfinder tracks controller movement
- [ ] Viewfinder size is adjustable
- [ ] Passepartout is adjustable

### Live Mode
- [ ] Lens + increases focal length
- [ ] Lens - decreases focal length
- [ ] Aperture + cycles to next f-stop
- [ ] Aperture - cycles to previous f-stop
- [ ] Toggle DOF turns depth of field on/off
- [ ] Capture creates new landmark
- [ ] Landmarks are auto-named correctly
- [ ] Camera settings are stored in landmark

### Playback Mode
- [ ] Mode switch updates UI
- [ ] Next button advances to next shot
- [ ] Previous button goes to previous shot
- [ ] Preview teleports to shot position
- [ ] Delete removes selected shot
- [ ] Navigation wraps at list ends

### Export
- [ ] Export creates camera keyframes
- [ ] Keyframe spacing is correct (24 frames)
- [ ] Camera position keyframes work
- [ ] Camera rotation keyframes work
- [ ] Lens keyframes work
- [ ] DOF keyframes work (if enabled)

### UI
- [ ] Director Viewfinder panel appears
- [ ] Enable checkbox works
- [ ] Mode selector updates controls
- [ ] All buttons are clickable
- [ ] Landmarks panel shows camera settings
- [ ] Settings sliders work

---

## 🐛 Common Issues & Solutions

### Viewfinder Not Appearing
**Problem**: Can't see viewfinder on controller  
**Solutions**:
- Check "Enable VR Viewfinder" checkbox is checked
- Verify VR session is running
- Check "Viewport Feedback" panel > "Show Controllers" is enabled
- Try toggling viewfinder off and on again

### Operators Disabled/Greyed Out
**Problem**: Buttons are disabled  
**Solutions**:
- Ensure VR session is active
- Check viewfinder is enabled
- Verify correct mode (Live vs Playback)
- For playback: ensure viewfinder shots exist

### Capture Doesn't Work
**Problem**: Clicking Capture does nothing  
**Solutions**:
- Ensure in Live mode
- Check an active camera exists
- Verify VR session is running
- Check console for error messages

### Export Has No Effect
**Problem**: Export button does nothing  
**Solutions**:
- Ensure at least one viewfinder shot exists
- Check an active camera is set (Ctrl+Numpad 0)
- Look in Timeline for keyframes
- Check console for messages

### Teleport/Preview Doesn't Work
**Problem**: Preview doesn't move viewer  
**Solutions**:
- Ensure in Playback mode
- Check a viewfinder landmark is selected
- Verify landmark type is "Viewfinder"
- Try activating landmark manually

---

## 📊 Test Report Template

After testing, fill out this report:

```
VR DIRECTOR VIEWFINDER - TEST REPORT
====================================

Date: _______________
Tester: _______________
System: _______________
VR Headset: _______________
Blender Version: _______________

VALIDATION TEST:
[ ] test_viewfinder.py passed

VR SESSION:
[ ] Started successfully
[ ] Controllers visible
[ ] Viewfinder appeared

LIVE MODE:
[ ] Lens adjustment works
[ ] Aperture cycling works
[ ] DOF toggle works
[ ] Capture creates landmarks
[ ] Settings stored correctly

PLAYBACK MODE:
[ ] Navigation works
[ ] Preview teleports correctly
[ ] Delete removes shots

EXPORT:
[ ] Keyframes created
[ ] Animation plays correctly
[ ] Camera data accurate

ISSUES FOUND:
1. 
2. 
3. 

SUGGESTIONS:
1. 
2. 
3. 

OVERALL RATING: ___/10

READY FOR MERGE: [ ] Yes [ ] No [ ] With changes
```

---

## 📝 Next Steps After Testing

### If Tests Pass ✅
1. Fill out test report
2. Document any observations
3. Prepare for code review
4. Submit for merge consideration

### If Issues Found ❌
1. Document each issue clearly
2. Include reproduction steps
3. Capture screenshots/videos if possible
4. File bug reports with details
5. Propose fixes or workarounds

---

## 📚 Additional Resources

- **Full Documentation**: `VR_VIEWFINDER_README.md`
- **Implementation Details**: `IMPLEMENTATION_SUMMARY.md`
- **Commit Message**: `COMMIT_MESSAGE.txt`
- **Validation Script**: `test_viewfinder.py`
- **Issue Tracker**: https://projects.blender.org/blender/blender/issues/141089

---

## 💡 Tips for Best Results

1. **Clear Play Area**: Ensure safe VR space
2. **Good Lighting**: Helps with tracking
3. **Charged Controllers**: Avoid disconnects
4. **Test Systematically**: Follow checklist order
5. **Document Everything**: Screenshots help debugging
6. **Try Edge Cases**: Test limits and boundaries
7. **Real Scenes**: Test with actual production content

---

## 🎉 You're Ready!

Follow this guide step-by-step to thoroughly test the VR Director Viewfinder implementation. Take your time, document issues, and enjoy exploring this new VR filmmaking tool!

**Happy Testing! 🚀**
