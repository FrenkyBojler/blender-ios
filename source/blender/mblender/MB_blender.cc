/**
 *
 */

#include "MB_patches.h"

#include <boost/preprocessor/if.hpp>

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

  return;
}

void MB_init(void) {
  MB_patches_discover();
}

char** MB_patches_get() {
  return (char**) patches;
}

void MB_print_info()
{
  printf("%s", "Mechanical Blender Info\n");
  printf("%s", "---------------------\n");
  for (int i = 0; i < MAX_MB_PATCHES; i++) {
    if (*patches[i] != '\0') {
      printf("Applied Patch %s\n", patches[i]);
    }
  }
  printf("%s", "---------------------\n");
}

