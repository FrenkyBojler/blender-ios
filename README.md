# Sculptcore

This branch is a proof of concept harness for integrating sculptcore, a new sculpting engine, 
into blender.  It has the minimal modification to integrate sculptcore as an addon.  
Claude Code was used for much of the coding, AI context lives in the claudeMemory folder
(which replaces the docs folder in this branch).  If this branch was ever submitted for
code review all AI context-related changes would be reverted (i.e. the claudeMemory folder).

Sculptcore has a few improvements over blender's sculpt code:

+ Uses a paged struct-of-arrays mesh structure with full BREP support (BREP 
  "pointers" can be partially stripped if needed).
+ BREP "pointers" are 32 bit integers to save memory (and more 
  importantly memory *bandwidth*) compared to BMesh.
+ Multires and VDMs are completely separate concepts; Blender's conflation of those
  two things is what causes the infamous numerical instability (multires spikes) it'saves
  long suffered from.
+ Sculptcore's dyntopo has full attribute interpolation and boundary constraint support.
  (so it supports dyntopo on UV-mapped meshes for example).
+ GPU compute support, currently mostly used for global brushes like elastic deform (kelvinlet) or grab.

## Integration ToDos

[ ]: Get the GPU brush compute path working.  Will likely require using dawn to transpile 
     shaders to glsl and whatever metal uses.  Note: Sculptcore's brush compute is based
	 on webgpu's wgsl so it already has infrastructure to compile to vulkan's spir-v.
[ ]: Make UI match existing sculpt mode as much as possible.

