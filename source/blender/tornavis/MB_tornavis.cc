/**
 *
 */

#include "MB_patches.h"

#include <boost/preprocessor/if.hpp>

#include <BLI_assert.h>

#include <stdio.h>
#include <string.h>

char patches[MAX_MB_PATCHES][8] = {0};

void MB_patches_discover()
{
  int i = 0;

  BOOST_PP_IF(MB_0001_APPLIED, strcpy(patches[i++], "MB_0001"), );
  BOOST_PP_IF(MB_0002_APPLIED, strcpy(patches[i++], "MB_0002"), );
  BOOST_PP_IF(MB_0003_APPLIED, strcpy(patches[i++], "MB_0003"), );
  BOOST_PP_IF(MB_0004_APPLIED, strcpy(patches[i++], "MB_0004"), );
  BOOST_PP_IF(MB_0005_APPLIED, strcpy(patches[i++], "MB_0005"), );
  BOOST_PP_IF(MB_0006_APPLIED, strcpy(patches[i++], "MB_0006"), );
  BOOST_PP_IF(MB_0007_APPLIED, strcpy(patches[i++], "MB_0007"), );
  BOOST_PP_IF(MB_0008_APPLIED, strcpy(patches[i++], "MB_0008"), );
  BOOST_PP_IF(MB_0009_APPLIED, strcpy(patches[i++], "MB_0009"), );
  BOOST_PP_IF(MB_0010_APPLIED, strcpy(patches[i++], "MB_0010"), );
  BOOST_PP_IF(MB_0011_APPLIED, strcpy(patches[i++], "MB_0011"), );
  BOOST_PP_IF(MB_0012_APPLIED, strcpy(patches[i++], "MB_0012"), );
  BOOST_PP_IF(MB_0013_APPLIED, strcpy(patches[i++], "MB_0013"), );
  BOOST_PP_IF(MB_0014_APPLIED, strcpy(patches[i++], "MB_0014"), );
  BOOST_PP_IF(MB_0015_APPLIED, strcpy(patches[i++], "MB_0015"), );
  BOOST_PP_IF(MB_0016_APPLIED, strcpy(patches[i++], "MB_0016"), );
  BOOST_PP_IF(MB_0017_APPLIED, strcpy(patches[i++], "MB_0017"), );
  BOOST_PP_IF(MB_0018_APPLIED, strcpy(patches[i++], "MB_0018"), );
  BOOST_PP_IF(MB_0019_APPLIED, strcpy(patches[i++], "MB_0019"), );
  BOOST_PP_IF(MB_0020_APPLIED, strcpy(patches[i++], "MB_0020"), );

  // Not necessary becuase initialitzed to {0}
  strcpy(patches[i++], "\0");

  return;
}

void MB_init(void)
{
  MB_patches_discover();
}

char *MB_patch_get(int pos)
{
  BLI_assert(pos < MAX_MB_PATCHES);
  return *patches[pos] == '\0' ? nullptr : patches[pos];
}

void MB_print_info()
{
  printf("%s", "Tornavis Info\n");
  printf("%s", "---------------------\n");
  for (int i = 0; i < MAX_MB_PATCHES; i++) {
    if (*patches[i] == '\0') {
      break;
    }
    printf("Applied Patch %s\n", patches[i]);
  }
  printf("%s", "---------------------\n");
}
