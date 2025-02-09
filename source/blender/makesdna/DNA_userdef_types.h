/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_anim_enums.h"
#include "DNA_asset_types.h"
#include "DNA_colorband_types.h"
#include "DNA_curve_enums.h"
#include "DNA_listBase.h"
#include "DNA_scene_enums.h"
#include "DNA_space_enums.h"
#include "DNA_theme_types.h"   /* IWYU pragma: export */
#include "DNA_userdef_enums.h" /* IWYU pragma: export */

#include "BLI_math_constants.h"

struct ColorBand;
struct IDProperty;

typedef struct bAddon {
  struct bAddon *next = nullptr, *prev = nullptr;
  /**
   * 64 characters for a package prefix, 63 characters for the add-on name.
   */
  char module[128] = "";
  /** User-Defined Properties on this add-on (for storing preferences). */
  struct IDProperty *prop = nullptr;
} bAddon;

typedef struct bPathCompare {
  struct bPathCompare *next = nullptr, *prev = nullptr;
  /** FILE_MAXDIR. */
  char path[768] = "";
  char flag = 0;
  char _pad0[7] = {};
} bPathCompare;

typedef struct bUserMenu {
  struct bUserMenu *next = nullptr, *prev = nullptr;
  char space_type = 0;
  char _pad0[7] = {};
  char context[64] = "";
  /* bUserMenuItem */
  ListBase items = {nullptr, nullptr};
} bUserMenu;

/** May be part of #bUserMenu or other list. */
typedef struct bUserMenuItem {
  struct bUserMenuItem *next = nullptr, *prev = nullptr;
  char ui_name[64] = "";
  char type = 0;
  char _pad0[7] = {};
} bUserMenuItem;

typedef struct bUserMenuItem_Op {
  bUserMenuItem item;
  char op_idname[64] = "";
  struct IDProperty *prop = nullptr;
  char op_prop_enum[64] = "";
  char opcontext = 0; /* #wmOperatorCallContext */
  char _pad0[7] = {};
} bUserMenuItem_Op;

typedef struct bUserMenuItem_Menu {
  bUserMenuItem item;
  char mt_idname[64] = "";
} bUserMenuItem_Menu;

typedef struct bUserMenuItem_Prop {
  bUserMenuItem item;
  char context_data_path[256] = "";
  char prop_id[64] = "";
  int prop_index = 0;
  char _pad0[4] = {};
} bUserMenuItem_Prop;

enum {
  USER_MENU_TYPE_SEP = 1,
  USER_MENU_TYPE_OPERATOR = 2,
  USER_MENU_TYPE_MENU = 3,
  USER_MENU_TYPE_PROP = 4,
};

typedef struct bUserAssetLibrary {
  struct bUserAssetLibrary *next = nullptr, *prev = nullptr;

  char name[64] = "";      /* MAX_NAME */
  char dirpath[1024] = ""; /* FILE_MAX */

  short import_method = ASSET_IMPORT_APPEND_REUSE; /* eAssetImportMethod */
  short flag = ASSET_LIBRARY_RELATIVE_PATH;        /* eAssetLibrary_Flag */
  char _pad0[4] = {};
} bUserAssetLibrary;

typedef struct bUserExtensionRepo {
  struct bUserExtensionRepo *next = nullptr, *prev = nullptr;
  /**
   * Unique identifier, only for display in the UI list.
   * The `module` is used for internal identifiers.
   */
  char name[64] = {'\0'}; /* MAX_NAME */
  /**
   * The unique module name (sub-module) in fact.
   *
   * Use a shorter name than #NAME_MAX to leave room for a base module prefix.
   * e.g. `bl_ext.{submodule}.{add_on}` to allow this string to fit into #bAddon::module.
   */
  char module[48] = {'\0'};

  /**
   * Secret access token for remote repositories (allocated).
   * Only use when #USER_EXTENSION_REPO_FLAG_USE_ACCESS_TOKEN is set.
   */
  char *access_token = nullptr;

  /**
   * The "local" directory where extensions are stored.
   * When unset, use `{BLENDER_USER_EXTENSIONS}/{bUserExtensionRepo::module}`.
   */
  char custom_dirpath[1024] = {'\0'}; /* FILE_MAX */
  char remote_url[1024] = {'\0'};     /* FILE_MAX */

  /** Options for the repository (#eUserExtensionRepo_Flag). */
  uint8_t flag = 0;
  /** The source location when the custom directory isn't used (#eUserExtensionRepo_Source). */
  uint8_t source = 0;

  char _pad0[6] = {};
} bUserExtensionRepo;

typedef enum eUserExtensionRepo_Flag {
  /** Maintain disk cache. */
  USER_EXTENSION_REPO_FLAG_NO_CACHE = 1 << 0,
  USER_EXTENSION_REPO_FLAG_DISABLED = 1 << 1,
  USER_EXTENSION_REPO_FLAG_USE_CUSTOM_DIRECTORY = 1 << 2,
  USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL = 1 << 3,
  USER_EXTENSION_REPO_FLAG_SYNC_ON_STARTUP = 1 << 4,
  USER_EXTENSION_REPO_FLAG_USE_ACCESS_TOKEN = 1 << 5,
} eUserExtensionRepo_Flag;

/**
 * The source to use (User or System), only valid when the
 * #USER_EXTENSION_REPO_FLAG_USE_REMOTE_URL flag isn't set.
 */
typedef enum eUserExtensionRepo_Source {
  USER_EXTENSION_REPO_SOURCE_USER = 0,
  USER_EXTENSION_REPO_SOURCE_SYSTEM = 1,
} eUserExtensionRepo_Source;

typedef struct SolidLight {
  int flag = 0;
  float smooth = 0;
  float col[4] = {}, spec[4] = {}, vec[4] = {};
} SolidLight;

typedef struct WalkNavigation {
  /** Speed factor for look around. */
  float mouse_speed = 1;
  float walk_speed = 2.5;
  float walk_speed_factor = 5;
  float view_height = 1.6;
  float jump_height = 0.4;
  /** Duration to use for teleporting. */
  float teleport_time = 0.2;
  short flag = 0;
  char _pad0[6] = {};
} WalkNavigation;

typedef struct UserDef_Runtime {
  /** Mark as changed so the preferences are saved on exit. */
  char is_dirty = 0;
  char _pad0[7] = {};
} UserDef_Runtime;

/** #UserDef_SpaceData.section_active (UI active_section) */
typedef enum eUserPref_Section {
  USER_SECTION_INTERFACE = 0,
  USER_SECTION_EDITING = 1,
  USER_SECTION_SAVE_LOAD = 2,
  USER_SECTION_SYSTEM = 3,
  USER_SECTION_THEME = 4,
  USER_SECTION_INPUT = 5,
  USER_SECTION_ADDONS = 6,
  USER_SECTION_LIGHT = 7,
  USER_SECTION_KEYMAP = 8,
#ifdef WITH_USERDEF_WORKSPACES
  USER_SECTION_WORKSPACE_CONFIG = 9,
  USER_SECTION_WORKSPACE_ADDONS = 10,
  USER_SECTION_WORKSPACE_KEYMAPS = 11,
#endif
  USER_SECTION_VIEWPORT = 12,
  USER_SECTION_ANIMATION = 13,
  USER_SECTION_NAVIGATION = 14,
  USER_SECTION_FILE_PATHS = 15,
  USER_SECTION_EXPERIMENTAL = 16,
  USER_SECTION_EXTENSIONS = 17,
} eUserPref_Section;

/** #UserDef_SpaceData.flag (State of the user preferences UI). */
typedef enum eUserPref_SpaceData_Flag {
  /** Hide/expand key-map preferences. */
  USER_SPACEDATA_INPUT_HIDE_UI_KEYCONFIG = (1 << 0),
  USER_SPACEDATA_ADDONS_SHOW_ONLY_ENABLED = (1 << 1),
} eUserPref_SpaceData_Flag;

/**
 * Store UI data here instead of the space
 * since the space is typically a window which is freed.
 */
typedef struct UserDef_SpaceData {
  char section_active = USER_SECTION_INTERFACE;
  /** #eUserPref_SpaceData_Flag UI options. */
  char flag = 0;
  char _pad0[6] = {};
} UserDef_SpaceData;

/**
 * Storage for UI data that to keep it even after the window was closed. (Similar to
 * #UserDef_SpaceData.)
 */
typedef struct UserDef_FileSpaceData {
  int display_type = FILE_VERTICALDISPLAY; /* FileSelectParams.display */
  int thumbnail_size = 96;                 /* FileSelectParams.thumbnail_size */
  int sort_type = FILE_SORT_ALPHA;         /* FileSelectParams.sort */
  int details_flags = FILE_DETAILS_SIZE |
                      FILE_DETAILS_DATETIME; /* FileSelectParams.details_flags */
  int flag = FILE_HIDE_DOT;                  /* FileSelectParams.flag */
  int _pad0 = 0;
  uint64_t filter_id = FILTER_ID_ALL; /* FileSelectParams.filter_id */

  /** Info used when creating the file browser in a temporary window. */
  int temp_win_sizex = 1060;
  int temp_win_sizey = 600;
} UserDef_FileSpaceData;

/**
 * Checking experimental members must use the #USER_EXPERIMENTAL_TEST() macro
 * unless the #USER_DEVELOPER_UI is known to be enabled.
 */
typedef struct UserDef_Experimental {
  /* Debug options, always available. */
  char use_undo_legacy = 0;
  char no_override_auto_resync = 0;
  char use_cycles_debug = 0;
  char use_eevee_debug = 0;
  char show_asset_debug_info = 0;
  char no_asset_indexing = 0;
  char use_viewport_debug = 0;
  char use_all_linked_data_direct = 0;
  char use_extensions_debug = 0;
  char use_recompute_usercount_on_save_debug = 0;
  char SANITIZE_AFTER_HERE = 0;
  /* The following options are automatically sanitized (set to 0)
   * when the release cycle is not alpha. */
  char use_new_curves_tools = 0;
  char use_new_point_cloud_type = 0;
  char use_sculpt_tools_tilt = 0;
  char use_extended_asset_browser = 0;
  char use_sculpt_texture_paint = 0;
  char use_new_volume_nodes = 0;
  char use_new_file_import_nodes = 0;
  char use_shader_node_previews = 0;
  char _pad[5] = {};
} UserDef_Experimental;

#define USER_EXPERIMENTAL_TEST(userdef, member) \
  (((userdef)->flag & USER_DEVELOPER_UI) && ((userdef)->experimental).member)

/**
 * Container to store multiple directory paths and a name for each as a #ListBase.
 */
typedef struct bUserScriptDirectory {
  struct bUserScriptDirectory *next = nullptr, *prev = nullptr;

  /** Name must be unique. */
  char name[64] = "";      /* MAX_NAME */
  char dir_path[768] = ""; /* FILE_MAXDIR */
} bUserScriptDirectory;

/**
 * Settings for an asset shelf, stored in the Preferences. Most settings are still stored in the
 * asset shelf instance in #AssetShelfSettings. This is just for the options that should be shared
 * as Preferences.
 */
typedef struct bUserAssetShelfSettings {
  struct bUserAssetShelfSettings *next = nullptr, *prev = nullptr;

  /** Identifier that matches the #AssetShelfType.idname of the shelf these settings apply to. */
  char shelf_idname[64] = {'\0'}; /* MAX_NAME */

  ListBase enabled_catalog_paths = {nullptr, nullptr}; /* #AssetCatalogPathLink */
} bUserAssetShelfSettings;

/* ***************** USERDEF ****************** */

/* Toggles for unfinished 2.8 UserPref design. */
// #define WITH_USERDEF_WORKSPACES

/** NOTE: Keep in sync with eGPUBackendType. */
enum eUserPref_GPUBackendType {
  USER_GPU_BACKEND_OPENGL = 1 << 0,
  USER_GPU_BACKEND_METAL = 1 << 1,
  USER_GPU_BACKEND_VULKAN = 1 << 3,
#ifdef __APPLE__
  USER_GPU_BACKEND_DEFAULT = USER_GPU_BACKEND_METAL,
#else
  USER_GPU_BACKEND_DEFAULT = USER_GPU_BACKEND_OPENGL,
#endif
};

/** #UserDef.flag */
typedef enum eUserPref_Flag {
  USER_AUTOSAVE = (1 << 0),
  USER_FLAG_NUMINPUT_ADVANCED = (1 << 1),
  USER_FLAG_RECENT_SEARCHES_DISABLE = (1 << 2),
  USER_FLAG_UNUSED_3 = (1 << 3), /* cleared */
  USER_FLAG_UNUSED_4 = (1 << 4), /* cleared */
  USER_TRACKBALL = (1 << 5),
  USER_FLAG_UNUSED_6 = (1 << 6), /* cleared */
  USER_FLAG_UNUSED_7 = (1 << 7), /* cleared */
  USER_MAT_ON_OB = (1 << 8),
  USER_INTERNET_ALLOW = (1 << 9),
  USER_DEVELOPER_UI = (1 << 10),
  USER_TOOLTIPS = (1 << 11),
  USER_TWOBUTTONMOUSE = (1 << 12),
  USER_NONUMPAD = (1 << 13),
  USER_ADD_CURSORALIGNED = (1 << 14),
  USER_FILECOMPRESS = (1 << 15),
  USER_FLAG_UNUSED_5 = (1 << 16), /* dirty */
  USER_CUSTOM_RANGE = (1 << 17),
  USER_ADD_EDITMODE = (1 << 18),
  USER_ADD_VIEWALIGNED = (1 << 19),
  USER_RELPATHS = (1 << 20),
  USER_RELEASECONFIRM = (1 << 21),
  USER_SCRIPT_AUTOEXEC_DISABLE = (1 << 22),
  USER_FILENOUI = (1 << 23),
  USER_NONEGFRAMES = (1 << 24),
  USER_TXT_TABSTOSPACES_DISABLE = (1 << 25),
  USER_TOOLTIPS_PYTHON = (1 << 26),
  USER_FLAG_UNUSED_27 = (1 << 27), /* dirty */
} eUserPref_Flag;

/** #UserDef.extension_flag */
typedef enum eUserPref_ExtensionFlag {
  USER_EXTENSION_FLAG_ONLINE_ACCESS_HANDLED = 1 << 0,
} eUserPref_ExtensionFlag;

/** #UserDef.file_preview_type */
typedef enum eUserpref_File_Preview_Type {
  USER_FILE_PREVIEW_NONE = 0,
  USER_FILE_PREVIEW_AUTO,
  USER_FILE_PREVIEW_SCREENSHOT,
  USER_FILE_PREVIEW_CAMERA,
} eUserpref_File_Preview_Type;

typedef enum eUserPref_PrefFlag {
  USER_PREF_FLAG_SAVE = (1 << 0),
} eUserPref_PrefFlag;

/** #bPathCompare.flag */
typedef enum ePathCompare_Flag {
  USER_PATHCMP_GLOB = (1 << 0),
} ePathCompare_Flag;

/* Helper macro for checking frame clamping */
#define FRAMENUMBER_MIN_CLAMP(cfra) \
  { \
    if ((U.flag & USER_NONEGFRAMES) && (cfra < 0)) { \
      cfra = 0; \
    } \
  } \
  (void)0

/** #UserDef.viewzoom */
typedef enum eViewZoom_Style {
  /** Update zoom continuously with a timer while dragging the cursor. */
  USER_ZOOM_CONTINUE = 0,
  /** Map changes in distance from the view center to zoom. */
  USER_ZOOM_SCALE = 1,
  /** Map horizontal/vertical motion to zoom. */
  USER_ZOOM_DOLLY = 2,
} eViewZoom_Style;

/** #UserDef.navigation_mode */
typedef enum eViewNavigation_Method {
  VIEW_NAVIGATION_WALK = 0,
  VIEW_NAVIGATION_FLY = 1,
} eViewNavigation_Method;

/** #UserDef.uiflag */
typedef enum eUserpref_MiniAxisType {
  USER_MINI_AXIS_TYPE_GIZMO = 0,
  USER_MINI_AXIS_TYPE_MINIMAL = 1,
  USER_MINI_AXIS_TYPE_NONE = 2,
} eUserpref_MiniAxisType;

/** #UserDef.flag */
typedef enum eWalkNavigation_Flag {
  USER_WALK_GRAVITY = (1 << 0),
  USER_WALK_MOUSE_REVERSE = (1 << 1),
} eWalkNavigation_Flag;

/** #UserDef.uiflag */
typedef enum eUserpref_UI_Flag {
  USER_NO_MULTITOUCH_GESTURES = (1 << 0),
  USER_UIFLAG_UNUSED_1 = (1 << 1), /* cleared */
  USER_WHEELZOOMDIR = (1 << 2),
  USER_FILTERFILEEXTS = (1 << 3),
  USER_DRAWVIEWINFO = (1 << 4),
  USER_PLAINMENUS = (1 << 5),
  USER_LOCK_CURSOR_ADJUST = (1 << 6),
  USER_HEADER_BOTTOM = (1 << 7),
  /** Otherwise use header alignment from the file. */
  USER_HEADER_FROM_PREF = (1 << 8),
  USER_MENUOPENAUTO = (1 << 9),
  USER_DEPTH_CURSOR = (1 << 10),
  USER_AUTOPERSP = (1 << 11),
  USER_NODE_AUTO_OFFSET = (1 << 12),
  USER_GLOBALUNDO = (1 << 13),
  USER_ORBIT_SELECTION = (1 << 14),
  USER_DEPTH_NAVIGATE = (1 << 15),
  USER_HIDE_DOT = (1 << 16),
  USER_SHOW_GIZMO_NAVIGATE = (1 << 17),
  USER_SHOW_VIEWPORTNAME = (1 << 18),
  USER_UIFLAG_UNUSED_3 = (1 << 19), /* Cleared. */
  USER_ZOOM_TO_MOUSEPOS = (1 << 20),
  USER_SHOW_FPS = (1 << 21),
  USER_REGISTER_ALL_USERS = (1 << 22),
  /** Actually implemented in .py. */
  USER_FILTER_BRUSHES_BY_TOOL = (1 << 23),
  USER_CONTINUOUS_MOUSE = (1 << 24),
  USER_ZOOM_INVERT = (1 << 25),
  USER_ZOOM_HORIZ = (1 << 26), /* for CONTINUE and DOLLY zoom */
  USER_SPLASH_DISABLE = (1 << 27),
  USER_HIDE_RECENT = (1 << 28),
#ifdef DNA_DEPRECATED_ALLOW
  /* Deprecated: We're just trying if there's much desire for this feature,
   * or if we can make it go for good. Should be cleared if so - Julian, Oct. 2019. */
  USER_SHOW_THUMBNAILS = (1 << 29),
#endif
  USER_SAVE_PROMPT = (1 << 30),
  USER_HIDE_SYSTEM_BOOKMARKS = (1u << 31),
} eUserpref_UI_Flag;

/**
 * #UserDef.uiflag2
 *
 * \note don't add new flags here, use 'uiflag' which has flags free.
 */
typedef enum eUserpref_UI_Flag2 {
  USER_UIFLAG2_UNUSED_0 = (1 << 0), /* cleared */
  USER_REGION_OVERLAP = (1 << 1),
  USER_UIFLAG2_UNUSED_2 = (1 << 2),
  USER_UIFLAG2_UNUSED_3 = (1 << 3), /* dirty */
} eUserpref_UI_Flag2;

/** #UserDef.gpu_flag */
typedef enum eUserpref_GPU_Flag {
  USER_GPU_FLAG_NO_DEPT_PICK = (1 << 0), /* Unused. To be removed. */
  USER_GPU_FLAG_NO_EDIT_MODE_SMOOTH_WIRE = (1 << 1),
  USER_GPU_FLAG_OVERLAY_SMOOTH_WIRE = (1 << 2),
  USER_GPU_FLAG_SUBDIVISION_EVALUATION = (1 << 3),
  USER_GPU_FLAG_FRESNEL_EDIT = (1 << 4),
} eUserpref_GPU_Flag;

/** #UserDef.tablet_api */
typedef enum eUserpref_TableAPI {
  USER_TABLET_AUTOMATIC = 0,
  USER_TABLET_NATIVE = 1,
  USER_TABLET_WINTAB = 2,
} eUserpref_TabletAPI;

/** #UserDef.app_flag */
typedef enum eUserpref_APP_Flag {
  USER_APP_LOCK_CORNER_SPLIT = (1 << 0),
  USER_APP_HIDE_REGION_TOGGLE = (1 << 1),
  USER_APP_LOCK_EDGE_RESIZE = (1 << 2),
} eUserpref_APP_Flag;

/** #UserDef.statusbar_flag */
typedef enum eUserpref_StatusBar_Flag {
  STATUSBAR_SHOW_MEMORY = (1 << 0),
  STATUSBAR_SHOW_VRAM = (1 << 1),
  STATUSBAR_SHOW_STATS = (1 << 2),
  STATUSBAR_SHOW_VERSION = (1 << 3),
  STATUSBAR_SHOW_SCENE_DURATION = (1 << 4),
  STATUSBAR_SHOW_EXTENSIONS_UPDATES = (1 << 5),
} eUserpref_StatusBar_Flag;

/**
 * Zoom to frame mode.
 * #UserDef.view_frame_type
 */
typedef enum eZoomFrame_Mode {
  ZOOM_FRAME_MODE_KEEP_RANGE = 0,
  ZOOM_FRAME_MODE_SECONDS = 1,
  ZOOM_FRAME_MODE_KEYFRAMES = 2,
} eZoomFrame_Mode;

/**
 * Defines how keyframes are inserted.
 * Used for regular keying and auto-keying.
 * Not all of those flags are stored in the user preferences (U.keying_flag).
 * Some are stored on the scene (toolsettings.keying_flag).
 */
typedef enum eKeying_Flag {
  /* Settings used across manual and auto-keying. */
  KEYING_FLAG_VISUALKEY = (1 << 2),
  KEYING_FLAG_XYZ2RGB = (1 << 3),
  KEYING_FLAG_CYCLEAWARE = (1 << 8),

  /* Auto-key options. */
  AUTOKEY_FLAG_INSERTAVAILABLE = (1 << 0),
  AUTOKEY_FLAG_INSERTNEEDED = (1 << 1),
  AUTOKEY_FLAG_ONLYKEYINGSET = (1 << 6),
  AUTOKEY_FLAG_NOWARNING = (1 << 7),
  AUTOKEY_FLAG_LAYERED_RECORD = (1 << 10),

  /* Manual Keying options. */
  MANUALKEY_FLAG_INSERTNEEDED = (1 << 11),
} eKeying_Flag;

typedef enum eKeyInsertChannels {
  USER_ANIM_KEY_CHANNEL_LOCATION = (1 << 0),
  USER_ANIM_KEY_CHANNEL_ROTATION = (1 << 1),
  USER_ANIM_KEY_CHANNEL_SCALE = (1 << 2),
  USER_ANIM_KEY_CHANNEL_ROTATION_MODE = (1 << 3),
  USER_ANIM_KEY_CHANNEL_CUSTOM_PROPERTIES = (1 << 4),
} eKeyInsertChannels;

/**
 * Animation flags
 * #UserDef.animation_flag, used for animation flags that aren't covered by more specific flags
 * (like eKeying_Flag).
 */
typedef enum eUserpref_Anim_Flags {
  USER_ANIM_SHOW_CHANNEL_GROUP_COLORS = (1 << 0),
  USER_ANIM_ONLY_SHOW_SELECTED_CURVE_KEYS = (1 << 1),
  USER_ANIM_HIGH_QUALITY_DRAWING = (1 << 2),
} eUserpref_Anim_Flags;

/** #UserDef.transopts */
typedef enum eUserpref_Translation_Flags {
  USER_TR_TOOLTIPS = (1 << 0),
  USER_TR_IFACE = (1 << 1),
  USER_TR_REPORTS = (1 << 2),
  USER_TR_UNUSED_3 = (1 << 3),            /* cleared */
  USER_TR_UNUSED_4 = (1 << 4),            /* cleared */
  USER_DOTRANSLATE_DEPRECATED = (1 << 5), /* Deprecated in 2.83. */
  USER_TR_UNUSED_6 = (1 << 6),            /* cleared */
  USER_TR_UNUSED_7 = (1 << 7),            /* cleared */
  USER_TR_NEWDATANAME = (1 << 8),
} eUserpref_Translation_Flags;

/**
 * Text Editor options
 * #UserDef.text_flag
 */
typedef enum eTextEdit_Flags {
  USER_TEXT_EDIT_AUTO_CLOSE = (1 << 0),
} eTextEdit_Flags;

/**
 * Text draw options
 * #UserDef.text_render
 */
typedef enum eText_Draw_Options {
  USER_TEXT_DISABLE_AA = (1 << 0),

  USER_TEXT_HINTING_NONE = (1 << 1),
  USER_TEXT_HINTING_SLIGHT = (1 << 2),
  USER_TEXT_HINTING_FULL = (1 << 3),

  USER_TEXT_RENDER_SUBPIXELAA = (1 << 4),
} eText_Draw_Options;

/**
 * Grease Pencil Settings.
 * #UserDef.gp_settings
 */
typedef enum eGP_UserdefSettings {
  GP_PAINT_UNUSED_0 = (1 << 0),
} eGP_UserdefSettings;

enum {
  USER_GIZMO_DRAW = (1 << 0),
};

/**
 * Color Picker Types.
 * #UserDef.color_picker_type
 */
typedef enum eColorPicker_Types {
  USER_CP_CIRCLE_HSV = 0,
  USER_CP_SQUARE_SV = 1,
  USER_CP_SQUARE_HS = 2,
  USER_CP_SQUARE_HV = 3,
  USER_CP_CIRCLE_HSL = 4,
} eColorPicker_Types;

/**
 * Time-code display styles.
 * #UserDef.timecode_style
 */
typedef enum eTimecodeStyles {
  /**
   * As little info as is necessary to show relevant info with '+' to denote the frames
   * i.e. HH:MM:SS+FF, MM:SS+FF, SS+FF, or MM:SS.
   */
  USER_TIMECODE_MINIMAL = 0,
  /** Reduced SMPTE - (HH:)MM:SS:FF */
  USER_TIMECODE_SMPTE_MSF = 1,
  /** Full SMPTE - HH:MM:SS:FF */
  USER_TIMECODE_SMPTE_FULL = 2,
  /** Milliseconds for sub-frames - HH:MM:SS.sss. */
  USER_TIMECODE_MILLISECONDS = 3,
  /** Seconds only. */
  USER_TIMECODE_SECONDS_ONLY = 4,
  /**
   * Private (not exposed as generic choices) options.
   * milliseconds for sub-frames, SubRip format- HH:MM:SS,sss.
   */
  USER_TIMECODE_SUBRIP = 100,
} eTimecodeStyles;

/** #UserDef.ndof_flag (3D mouse options) */
typedef enum eNdof_Flag {
  NDOF_SHOW_GUIDE_ORBIT_AXIS = (1 << 0),
  NDOF_FLY_HELICOPTER = (1 << 1),
  NDOF_LOCK_HORIZON = (1 << 2),

  /* The following might not need to be saved between sessions,
   * but they do need to live somewhere accessible. */
  NDOF_SHOULD_PAN = (1 << 3),
  NDOF_SHOULD_ZOOM = (1 << 4),
  NDOF_SHOULD_ROTATE = (1 << 5),

  /* Orbit navigation modes. */

  NDOF_MODE_ORBIT = (1 << 6),

  /* actually... users probably don't care about what the mode
   * is called, just that it feels right */
  /* zoom is up/down if this flag is set (otherwise forward/backward) */
  NDOF_PAN_YZ_SWAP_AXIS = (1 << 7),
  NDOF_ZOOM_INVERT = (1 << 8),
  NDOF_ROTX_INVERT_AXIS = (1 << 9),
  NDOF_ROTY_INVERT_AXIS = (1 << 10),
  NDOF_ROTZ_INVERT_AXIS = (1 << 11),
  NDOF_PANX_INVERT_AXIS = (1 << 12),
  NDOF_PANY_INVERT_AXIS = (1 << 13),
  NDOF_PANZ_INVERT_AXIS = (1 << 14),
  NDOF_TURNTABLE = (1 << 15),
  NDOF_CAMERA_PAN_ZOOM = (1 << 16),
  NDOF_ORBIT_CENTER_AUTO = (1 << 17),
  NDOF_ORBIT_CENTER_SELECTED = (1 << 18),
  NDOF_SHOW_GUIDE_ORBIT_CENTER = (1 << 19),
} eNdof_Flag;

#define NDOF_PIXELS_PER_SECOND 600.0f

/** UserDef.ogl_multisamples */
typedef enum eMultiSample_Type {
  USER_MULTISAMPLE_NONE = 0,
  USER_MULTISAMPLE_2 = 2,
  USER_MULTISAMPLE_4 = 4,
  USER_MULTISAMPLE_8 = 8,
  USER_MULTISAMPLE_16 = 16,
} eMultiSample_Type;

/** #UserDef.image_draw_method */
typedef enum eImageDrawMethod {
  IMAGE_DRAW_METHOD_AUTO = 0,
  IMAGE_DRAW_METHOD_GLSL = 1,
  IMAGE_DRAW_METHOD_2DTEXTURE = 2,
} eImageDrawMethod;

/** #UserDef.virtual_pixel */
typedef enum eUserpref_VirtualPixel {
  VIRTUAL_PIXEL_NATIVE = 0,
  VIRTUAL_PIXEL_DOUBLE = 1,
} eUserpref_VirtualPixel;

/** #UserDef.factor_display_type */
typedef enum eUserpref_FactorDisplay {
  USER_FACTOR_AS_FACTOR = 0,
  USER_FACTOR_AS_PERCENTAGE = 1,
} eUserpref_FactorDisplay;

typedef enum eUserpref_RenderDisplayType {
  USER_RENDER_DISPLAY_NONE = 0,
  USER_RENDER_DISPLAY_SCREEN = 1,
  USER_RENDER_DISPLAY_AREA = 2,
  USER_RENDER_DISPLAY_WINDOW = 3
} eUserpref_RenderDisplayType;

typedef enum eUserpref_TempSpaceDisplayType {
  USER_TEMP_SPACE_DISPLAY_FULLSCREEN = 0,
  USER_TEMP_SPACE_DISPLAY_WINDOW = 1,
} eUserpref_TempSpaceDisplayType;

typedef enum eUserpref_EmulateMMBMod {
  USER_EMU_MMB_MOD_ALT = 0,
  USER_EMU_MMB_MOD_OSKEY = 1,
} eUserpref_EmulateMMBMod;

typedef enum eUserpref_TrackpadScrollDir {
  USER_TRACKPAD_SCROLL_DIR_TRADITIONAL = 0,
  USER_TRACKPAD_SCROLL_DIR_NATURAL = 1,
} eUserpref_TrackpadScrollDir;

typedef enum eUserpref_DiskCacheCompression {
  USER_SEQ_DISK_CACHE_COMPRESSION_NONE = 0,
  USER_SEQ_DISK_CACHE_COMPRESSION_LOW = 1,
  USER_SEQ_DISK_CACHE_COMPRESSION_HIGH = 2,
} eUserpref_DiskCacheCompression;

typedef enum eUserpref_SeqProxySetup {
  USER_SEQ_PROXY_SETUP_MANUAL = 0,
  USER_SEQ_PROXY_SETUP_AUTOMATIC = 1,
} eUserpref_SeqProxySetup;

typedef enum eUserpref_SeqEditorFlags {
  USER_SEQ_ED_SIMPLE_TWEAKING = (1 << 0),
  USER_SEQ_ED_CONNECT_STRIPS_BY_DEFAULT = (1 << 1),
} eUserpref_SeqEditorFlags;

/* Locale Ids. Auto will try to get local from OS. Our default is English though. */
/** #UserDef.language */
enum {
  ULANGUAGE_AUTO = 0,
  ULANGUAGE_ENGLISH = 1,
};
/**
 * Main user preferences data, typically accessed from #U.
 * See: #BKE_blendfile_userdef_from_defaults & #BKE_blendfile_userdef_read.
 *
 * \note This is either loaded from the file #BLENDER_USERPREF_FILE or from memory, see #U_default.
 */
typedef struct UserDef {
  DNA_DEFINE_CXX_METHODS(UserDef)

  /** UserDef has separate do-version handling, and can be read from other files. */
  int versionfile = 0;
  int subversionfile = 0;

  /** #eUserPref_Flag. */
  int flag = (USER_AUTOSAVE | USER_TOOLTIPS | USER_RELPATHS | USER_RELEASECONFIRM);
  /** #eDupli_ID_Flags. */
  unsigned int dupflag = USER_DUP_MESH | USER_DUP_CURVE | USER_DUP_SURF | USER_DUP_LATTICE |
                         USER_DUP_FONT | USER_DUP_MBALL | USER_DUP_LAMP | USER_DUP_ARM |
                         USER_DUP_CAMERA | USER_DUP_SPEAKER | USER_DUP_ACT | USER_DUP_LIGHTPROBE |
                         USER_DUP_GPENCIL | USER_DUP_CURVES | USER_DUP_POINTCLOUD;
  /** #eUserPref_PrefFlag preferences for the preferences. */
  char pref_flag = USER_PREF_FLAG_SAVE;
  char savetime = 2;
  char mouse_emulate_3_button_modifier = 0;
  /**
   * Workaround for WAYLAND (at time of writing compositors don't support this info).
   * #eUserpref_TrackpadScrollDir type
   * TODO: Remove this once this API is better supported by Wayland compositors, see #107676.
   */
  char trackpad_scroll_direction = 0;
  /** FILE_MAXDIR length. */
  char tempdir[768] = "";
  char fontdir[768] = "//";
  /** FILE_MAX length. */
  char renderdir[1024] = "//";
  /* EXR cache path */
  /** 768 = FILE_MAXDIR. */
  char render_cachedir[768] = "";
  char textudir[768] = "//";
  /* Deprecated, use #UserDef.script_directories instead. */
  char pythondir_legacy[768] DNA_DEPRECATED = {};
  char sounddir[768] = "//";
  char i18ndir[768] = "";
  /** 1024 = FILE_MAX. */
  char image_editor[1024] = "";
  /** 1024 = FILE_MAX. */
  char text_editor[1024] = "";
  char text_editor_args[256] = "";
  /** 1024 = FILE_MAX. */
  char anim_player[1024] = "";
  int anim_player_preset = 0;

  /** Minimum spacing between grid-lines in View2D grids. */
  short v2d_min_gridsize = 45;
  /** #eTimecodeStyles, style of time-code display. */
  short timecode_style = USER_TIMECODE_MINIMAL;

  short versions = 1;
  short dbl_click_time = 350;

  char _pad0[3] = {};
  char mini_axis_type = USER_MINI_AXIS_TYPE_GIZMO;
  /** #eUserpref_UI_Flag. */
  int uiflag = (USER_FILTERFILEEXTS | USER_DRAWVIEWINFO | USER_PLAINMENUS |
                USER_LOCK_CURSOR_ADJUST | USER_DEPTH_CURSOR | USER_AUTOPERSP |
                USER_NODE_AUTO_OFFSET | USER_GLOBALUNDO | USER_SHOW_GIZMO_NAVIGATE |
                USER_SHOW_VIEWPORTNAME | USER_SHOW_FPS | USER_CONTINUOUS_MOUSE | USER_SAVE_PROMPT);
  /** #eUserpref_UI_Flag2. */
  char uiflag2 = USER_REGION_OVERLAP;
  char gpu_flag = USER_GPU_FLAG_OVERLAY_SMOOTH_WIRE | USER_GPU_FLAG_SUBDIVISION_EVALUATION;
  char _pad8[6] = {};
  /* Experimental flag for app-templates to make changes to behavior
   * which are outside the scope of typical preferences. */
  char app_flag = 0;
  char viewzoom = USER_ZOOM_DOLLY;
  short language = 1;

  int mixbufsize = 2048;
  int audiodevice = 0;
  int audiorate = 48000;
  int audioformat = 0x24;
  int audiochannels = 2;

  /** Setting for UI scale (fractional), before screen DPI has been applied. */
  float ui_scale = 1.0;
  /** Setting for UI line width. */
  int ui_line_width = 0;
  /** Runtime, full DPI divided by `pixelsize`. */
  int dpi = 0;
  /** Runtime multiplier to scale UI elements. Use macro UI_SCALE_FAC instead of this. */
  float scale_factor = 0.0;
  /** Runtime, `1.0 / scale_factor` */
  float inv_scale_factor = 0.0; /* run-time. */
  /** Runtime, calculated from line-width and point-size based on DPI (rounded to int). */
  float pixelsize = 1;
  /** Deprecated, for forward compatibility. */
  int virtual_pixel = 0;

  /** Console scroll-back limit. */
  int scrollback = 256;
  /** Node insert offset (aka auto-offset) margin, but might be useful for later stuff as well. */
  char node_margin = 40;
  char node_preview_res = 120;
  /** #eUserpref_Translation_Flags. */
  short transopts = USER_TR_TOOLTIPS;
  short menuthreshold1 = 5, menuthreshold2 = 2;

  /** Startup application template. */
  char app_template[64] = "";

  /**
   * A list of themes (#bTheme), the first is only used currently.
   * But there may be multiple themes in the list.
   */
  struct ListBase themes = {nullptr, nullptr};
  struct ListBase uifonts = {nullptr, nullptr};
  struct ListBase uistyles = {nullptr, nullptr};
  struct ListBase user_keymaps = {nullptr, nullptr};
  /** #wmKeyConfigPref. */
  struct ListBase user_keyconfig_prefs = {nullptr, nullptr};
  struct ListBase addons = {nullptr, nullptr};
  struct ListBase autoexec_paths = {nullptr, nullptr};
  /**
   * Optional user locations for Python scripts.
   *
   * This supports the same layout as Blender's scripts directory `scripts`.
   *
   * \note Unlike most paths, changing this is not fully supported at run-time,
   * requiring a restart to properly take effect. Supporting this would cause complications as
   * the script path can contain `startup`, `addons` & `modules` etc. properly unwinding the
   * Python environment to the state it _would_ have been in gets complicated.
   *
   * Although this is partially supported as the `sys.path` is refreshed when loading preferences.
   * This is done to support #PREFERENCES_OT_copy_prev which is available to the user when they
   * launch with a new version of Blender. In this case setting the script path on top of
   * factory settings will work without problems.
   */
  ListBase script_directories = {nullptr, nullptr}; /* #bUserScriptDirectory */
  /** #bUserMenu. */
  struct ListBase user_menus = {nullptr, nullptr};
  /** #bUserAssetLibrary */
  struct ListBase asset_libraries = {nullptr, nullptr};
  /** #bUserExtensionRepo */
  struct ListBase extension_repos = {nullptr, nullptr};
  struct ListBase asset_shelves_settings = {nullptr, nullptr}; /* #bUserAssetShelfSettings */

  char keyconfigstr[64] = "Blender";

  /** Index of the asset library being edited in the Preferences UI. */
  short active_asset_library = 0;

  /** Index of the extension repo in the Preferences UI. */
  short active_extension_repo = 0;
  /** Flag for all extensions (#eUserPref_ExtensionFlag).  */
  char extension_flag = 0;

  /* Network settings, used by extensions but not specific to extensions. */

  /** Time in seconds to wait before timing out online operation (0 uses the systems default). */
  uint8_t network_timeout = 10;
  /** Maximum number of simulations connection limit for online operations. */
  uint8_t network_connection_limit = 5;

  char _pad14[3] = {};

  short undosteps = 32;
  int undomemory = 0;
  float gpu_viewport_quality DNA_DEPRECATED = 0;
  short gp_manhattandist = 1, gp_euclideandist = 2, gp_eraser = 25;
  /** #eGP_UserdefSettings. */
  short gp_settings = 0;
  char _pad13[4] = {};
  struct SolidLight light_param[4];
  float light_ambient[3] = {};
  char gizmo_flag = USER_GIZMO_DRAW;
  /** Generic gizmo size. */
  char gizmo_size = 75;
  /** Navigate gizmo size. */
  char gizmo_size_navigate_v3d = 80;
  char _pad3[5] = {};
  short edit_studio_light = 0;
  short lookdev_sphere_size = 150;
  short vbotimeout = 120, vbocollectrate = 60;
  short textimeout = 120, texcollectrate = 60;
  int memcachelimit = 4096;
  /** Unused. */
  int prefetchframes = 0;
  /** Control the rotation step of the view when PAD2, PAD4, PAD6&PAD8 is use. */
  float pad_rot_angle = 15;
  char _pad12[4] = {};
  /** Rotating view icon size. */
  short rvisize = 25;
  /** Rotating view icon brightness. */
  short rvibright = 8;
  /** Maximum number of recently used files to remember. */
  short recent_files = 20;
  /** Milliseconds to spend spinning the view. */
  short smooth_viewtx = 200;
  short glreslimit = 0;
  /** #eColorPicker_Types. */
  short color_picker_type = USER_CP_CIRCLE_HSV;
  /** Curve smoothing type for newly added F-Curves. */
  char auto_smoothing_new = FCURVE_SMOOTH_CONT_ACCEL;
  /** Interpolation mode for newly added F-Curves. */
  char ipo_new = BEZT_IPO_BEZ;
  /** Handle types for newly added keyframes. */
  char keyhandles_new = HD_AUTO_ANIM;
  char _pad11[4] = {};
  /** #eZoomFrame_Mode. */
  char view_frame_type = ZOOM_FRAME_MODE_KEEP_RANGE;

  /** Number of keyframes to zoom around current frame. */
  int view_frame_keyframes = 0;
  /** Seconds to zoom around current frame. */
  float view_frame_seconds = 0.0;

  /** Preferred device/vendor for GPU device selection. */
  int gpu_preferred_index = 0;
  uint32_t gpu_preferred_vendor_id = 0;
  uint32_t gpu_preferred_device_id = 0;
  char _pad16[4] = {};
  /** #eGPUBackendType */
  short gpu_backend = USER_GPU_BACKEND_DEFAULT;

  /** Max number of parallel shader compilation subprocesses. */
  short max_shader_compilation_subprocesses = 0;

  /** Number of samples for FPS display calculations. */
  short playback_fps_samples = 8;

  /** Private, defaults to 20 for 72 DPI setting. */
  short widget_unit = 0; /* run-time initialized. */
  short anisotropic_filter = 2;

  /** Tablet API to use (Windows only). */
  short tablet_api = USER_TABLET_AUTOMATIC;

  /** Raw tablet pressure that maps to 100%. */
  float pressure_threshold_max = 1.0;
  /** Curve non-linearity parameter. */
  float pressure_softness = 0.0;

  /** Overall sensitivity of 3D mouse. */
  float ndof_sensitivity = 4.0;
  float ndof_orbit_sensitivity = 4.0;
  /** Dead-zone of 3D mouse. */
  float ndof_deadzone = 0.0;
  /** #eNdof_Flag, flags for 3D mouse. */
  int ndof_flag = (NDOF_SHOW_GUIDE_ORBIT_CENTER | NDOF_ORBIT_CENTER_AUTO | NDOF_MODE_ORBIT |
                   NDOF_LOCK_HORIZON | NDOF_SHOULD_PAN | NDOF_SHOULD_ZOOM | NDOF_SHOULD_ROTATE |
                   /* Software from the driver authors follows this convention
                    * so invert this by default, see: #67579. */
                   NDOF_ROTX_INVERT_AXIS | NDOF_ROTY_INVERT_AXIS | NDOF_ROTZ_INVERT_AXIS |
                   NDOF_PANX_INVERT_AXIS | NDOF_PANY_INVERT_AXIS | NDOF_PANZ_INVERT_AXIS |
                   NDOF_ZOOM_INVERT | NDOF_CAMERA_PAN_ZOOM);

  /** #eMultiSample_Type, amount of samples for OpenGL FSA, if zero no FSA. */
  short ogl_multisamples = 0;

  /** eImageDrawMethod, Method to be used to draw the images
   * (AUTO, GLSL, Textures or DrawPixels) */
  short image_draw_method = IMAGE_DRAW_METHOD_AUTO;

  float glalphaclip = 0.004;

  /** #eAutokey_Mode, auto-keying mode. */
  short autokey_mode = (AUTOKEY_MODE_NORMAL & ~AUTOKEY_ON);
  /** Flags for inserting keyframes. */
  short keying_flag = KEYING_FLAG_XYZ2RGB | AUTOKEY_FLAG_INSERTNEEDED;
  /** Flags for which channels to insert keys at. */
  short key_insert_channels = (USER_ANIM_KEY_CHANNEL_LOCATION | USER_ANIM_KEY_CHANNEL_ROTATION |
                               USER_ANIM_KEY_CHANNEL_SCALE |
                               USER_ANIM_KEY_CHANNEL_CUSTOM_PROPERTIES);  // eKeyInsertChannels
  char _pad15[6] = {};
  /** Flags for animation. */
  short animation_flag = USER_ANIM_HIGH_QUALITY_DRAWING;

  /** Options for text rendering. */
  char text_render = 0;
  char navigation_mode = VIEW_NAVIGATION_WALK;

  /** Turn-table rotation amount per-pixel in radians. Scaled with DPI. */
  float view_rotate_sensitivity_turntable = 0.4 * M_PI / 180.0f;
  /** Track-ball rotation scale. */
  float view_rotate_sensitivity_trackball = 1.0f;

  /** From texture.h. */
  struct ColorBand coba_weight;

  float sculpt_paint_overlay_col[3] = {0, 0, 0};
  /** Default color for newly created Grease Pencil layers. */
  float gpencil_new_layer_col[4] = {0.38, 0.61, 0.78, 0.9};

  /** Drag pixels (scaled by DPI). */
  char drag_threshold_mouse = 3;
  char drag_threshold_tablet = 10;
  char drag_threshold = 30;
  char move_threshold = 2;

  char font_path_ui[1024] = "";
  char font_path_ui_mono[1024] = "";

  /** Legacy, for backwards compatibility only. */
  int compute_device_type = 0;

  /** Opacity of inactive F-Curves in F-Curve Editor. */
  float fcu_inactive_alpha = 0.25;

  /**
   * If keeping a pie menu spawn button pressed after this time,
   * it turns into a drag/release pie menu.
   */
  short pie_tap_timeout = 20;
  /**
   * Direction in the pie menu will always be calculated from the
   * initial position within this time limit.
   */
  short pie_initial_timeout = 0;
  short pie_animation_timeout = 6;
  short pie_menu_confirm = 0;
  /** Pie menu radius. */
  short pie_menu_radius = 100;
  /** Pie menu distance from center before a direction is set. */
  short pie_menu_threshold = 12;

  int sequencer_editor_flag = USER_SEQ_ED_SIMPLE_TWEAKING |
                              USER_SEQ_ED_CONNECT_STRIPS_BY_DEFAULT; /* eUserpref_SeqEditorFlags */

  char factor_display_type = USER_FACTOR_AS_FACTOR;

  char viewport_aa = 8;

  char render_display_type = USER_RENDER_DISPLAY_WINDOW; /* eUserpref_RenderDisplayType */
  char filebrowser_display_type =
      USER_TEMP_SPACE_DISPLAY_WINDOW; /* eUserpref_TempSpaceDisplayType */

  char sequencer_disk_cache_dir[1024] = "";
  int sequencer_disk_cache_compression = 0; /* eUserpref_DiskCacheCompression */
  int sequencer_disk_cache_size_limit = 100;
  short sequencer_disk_cache_flag = 0;
  short sequencer_proxy_setup = USER_SEQ_PROXY_SETUP_AUTOMATIC; /* eUserpref_SeqProxySetup */

  float collection_instance_empty_size = 1.0f;
  char text_flag = 0;
  char _pad10[1] = {};

  char file_preview_type = USER_FILE_PREVIEW_AUTO; /* eUserpref_File_Preview_Type */
  char statusbar_flag = STATUSBAR_SHOW_VERSION |
                        STATUSBAR_SHOW_EXTENSIONS_UPDATES; /* eUserpref_StatusBar_Flag */

  struct WalkNavigation walk_navigation;

  /** The UI for the user preferences. */
  UserDef_SpaceData space_data;
  UserDef_FileSpaceData file_space_data;

  UserDef_Experimental experimental;

  /** Runtime data (keep last). */
  UserDef_Runtime runtime;
} UserDef;

/** From `source/blender/blenkernel/intern/blender.cc`. */
extern UserDef U;
extern const UserDef U_default;
