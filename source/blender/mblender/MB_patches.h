#ifndef MB_BLENDER_PATCHES_H
#define MB_BLENDER_PATCHES_H

#ifdef MB_0001
  #include "patches/MB_0001.h"
  #define MB_0001_APPLIED 1
#else
  #define MB_0001_APPLIED 0
#endif

#ifdef MB_0002
  #include "patches/MB_0002.h"
#define MB_0002_APPLIED 1
#else
  #define MB_0002_APPLIED 0
#endif

#ifdef MB_0003
  #include "patches/MB_0003.h"
  #define MB_0003_APPLIED 1
#else
#  define MB_0003_APPLIED 0
#endif

#ifdef MB_0004
#  include "patches/MB_0004.h"
#  define MB_0004_APPLIED 1
#else
#  define MB_0004_APPLIED 0
#endif

#ifdef MB_0005
#  include "patches/MB_0005.h"
#  define MB_0005_APPLIED 1
#else
#  define MB_0005_APPLIED 0
#endif

#ifdef MB_0006
#  include "patches/MB_0005.h"
#  define MB_0006_APPLIED 1
#else
#  define MB_0006_APPLIED 0
#endif

#ifdef MB_0007
#  include "patches/MB_0005.h"
#  define MB_0007_APPLIED 1
#else
#  define MB_0007_APPLIED 0
#endif

#ifdef MB_0008
#  include "patches/MB_0005.h"
#  define MB_0008_APPLIED 1
#else
#  define MB_0008_APPLIED 0
#endif

#ifdef MB_0009
#  include "patches/MB_0005.h"
#  define MB_0009_APPLIED 1
#else
#  define MB_0009_APPLIED 0
#endif

#ifdef MB_0010
#  include "patches/MB_0010.h"
#  define MB_0010_APPLIED 1
#else
#  define MB_0010_APPLIED 0
#endif

#ifndef MB_0011
#define MAX_MB_PATCHES 12
#endif  // !MB_0004


#endif // !MB_BLENDER_PATCHES_H
