/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spfile
 * \brief macOS System File menu implementation.
 */

#import "Foundation/Foundation.h"

#include "BLI_fileops.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BLT_translation.hh"
#include "UI_resources.hh"

#include "fsmenu.hh"

struct FSMenu;

void fsmenu_macos_insert_entry(FSMenu *fsmenu,
                               const char *home_path,
                               const char *name,
                               const int icon)
{
  const char *home = BLI_dir_home();

  char path[FILE_MAXDIR];
  SNPRINTF(path, home_path, home);

  fsmenu_insert_entry(fsmenu, FS_CATEGORY_OTHER, path, name, icon, FS_INSERT_LAST);
}

void fsmenu_read_system(FSMenu *fsmenu, int read_bookmarks)
{
  /* We store some known macOS system paths and corresponding icons
   * and names in the FS_CATEGORY_OTHER (not displayed directly) category. */
  fsmenu_insert_entry(
      fsmenu, FS_CATEGORY_OTHER, "/Library/Fonts/", N_("Fonts"), ICON_FILE_FONT, FS_INSERT_LAST);
  fsmenu_insert_entry(fsmenu,
                      FS_CATEGORY_OTHER,
                      "/Applications/",
                      N_("Applications"),
                      ICON_FILE_FOLDER,
                      FS_INSERT_LAST);

  const char *home = BLI_dir_home();
  if (home) {
    fsmenu_macos_insert_entry(fsmenu, "%s/", nullptr, ICON_HOME);
    fsmenu_macos_insert_entry(fsmenu, "%s/Desktop/", N_("Desktop"), ICON_DESKTOP);
    fsmenu_macos_insert_entry(fsmenu, "%s/Documents/", N_("Documents"), ICON_DOCUMENTS);
    fsmenu_macos_insert_entry(fsmenu, "%s/Downloads/", N_("Downloads"), ICON_IMPORT);
    fsmenu_macos_insert_entry(fsmenu, "%s/Movies/", N_("Movies"), ICON_FILE_MOVIE);
    fsmenu_macos_insert_entry(fsmenu, "%s/Music/", N_("Music"), ICON_FILE_SOUND);
    fsmenu_macos_insert_entry(fsmenu, "%s/Pictures/", N_("Pictures"), ICON_FILE_IMAGE);
    fsmenu_macos_insert_entry(fsmenu, "%s/Library/Fonts/", N_("Fonts"), ICON_FILE_FONT);
  }

  NSFileManager *fileManager = [NSFileManager defaultManager];

  NSArray *resource_keys =
      @[ NSURLVolumeLocalizedNameKey, NSURLVolumeIsLocalKey, NSURLVolumeIsRemovableKey ];

  NSArray *mounted_volume_urls = [fileManager
      mountedVolumeURLsIncludingResourceValuesForKeys:resource_keys
                                              options:NSVolumeEnumerationSkipHiddenVolumes];

  for (NSURL *volume_url in mounted_volume_urls) {
    NSError *error = nil;
    NSDictionary *resources_values = [volume_url resourceValuesForKeys:resource_keys error:&error];

    if (!error) {
      NSString *volume_name = resources_values[NSURLVolumeLocalizedNameKey];
      NSNumber *volume_is_local = resources_values[NSURLVolumeIsLocalKey];
      NSNumber *volume_is_ejectable = resources_values[NSURLVolumeIsRemovableKey];

      /* Set icon for regular, removable or network drive. */
      int icon = ICON_DISK_DRIVE;
      if (!volume_is_local.boolValue) {
        icon = ICON_NETWORK_DRIVE;
      }
      if (volume_is_ejectable.boolValue) {
        icon = ICON_EXTERNAL_DRIVE;
      }

      fsmenu_insert_entry(fsmenu,
                          FS_CATEGORY_SYSTEM,
                          volume_url.path.UTF8String,
                          volume_name.UTF8String,
                          icon,
                          FS_INSERT_SORTED);
    }
  }

  /* The LSSharedFileList API has been deprecated, and no replacement has been provided to obtain
   * the user's Finder Favorites items from other applications. Ignore these deprecation warnings.
   * It is unknown when this API will be fully removed from macOS. */
  if (read_bookmarks) {
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    LSSharedFileListRef shared_list = LSSharedFileListCreate(
        nullptr, kLSSharedFileListFavoriteItems, nullptr);

    UInt32 seed;
    NSArray *paths_array = (__bridge NSArray *)LSSharedFileListCopySnapshot(shared_list, &seed);

    for (id item in paths_array) {
      LSSharedFileListItemRef item_ref = (__bridge LSSharedFileListItemRef)item;

      CFURLRef cf_url = nullptr;
      OSErr err = LSSharedFileListItemResolve(item_ref,
                                              kLSSharedFileListNoUserInteraction |
                                              kLSSharedFileListDoNotMountVolumes,
                                              &cf_url,
                                              nullptr);
      if (err != noErr || !cf_url) {
        continue;
      }

      NSURL *url = (__bridge NSURL *)cf_url;
      NSString *path = [url path];

      if (!path) {
        CFRelease(cf_url);
        continue;
      }

      // Exclude "all my files" and empty paths
      if (![path containsString:@"myDocuments.cannedSearch"] && [path length] > 0) {
        fsmenu_insert_entry(fsmenu, FS_CATEGORY_SYSTEM_BOOKMARKS,
                            [path UTF8String], nullptr, ICON_FILE_FOLDER, FS_INSERT_LAST);
      }

      CFRelease(cf_url);
    }

    [paths_array release];
    CFRelease(shared_list);
  }
#  pragma GCC diagnostic pop
}
