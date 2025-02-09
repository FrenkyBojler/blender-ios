/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
/** \file
 * \ingroup DNA
 *
 * Structs for each of space type in the user interface.
 */

#pragma once

#include "DNA_asset_types.h"
#include "DNA_color_types.h" /* for Histogram */
#include "DNA_defs.h"
#include "DNA_image_types.h" /* ImageUser */
#include "DNA_listBase.h"
#include "DNA_mask_types.h"
#include "DNA_movieclip_types.h" /* MovieClipUser */
#include "DNA_node_types.h"      /* for bNodeInstanceKey */
#include "DNA_outliner_types.h"  /* for TreeStoreElem */
#include "DNA_space_enums.h"
#include "DNA_vec_defaults.h"
/* Hum ... Not really nice... but needed for spacebuts. */
#include "DNA_view2d_types.h"
#include "DNA_viewer_path_types.h"

struct BLI_mempool;
struct FileLayout;
struct FileList;
struct FileSelectParams;
struct Histogram;
struct ID;
struct Image;
struct Mask;
struct MovieClip;
struct MovieClipScopes;
struct Scopes;
struct Script;
struct SpaceGraph;
struct Text;
struct bDopeSheet;
struct bGPdata;
struct bNodeTree;
struct wmOperator;
struct wmTimer;

#ifdef __cplusplus
namespace blender::asset_system {
class AssetRepresentation;
}
using AssetRepresentationHandle = blender::asset_system::AssetRepresentation;
#else
typedef struct AssetRepresentationHandle AssetRepresentationHandle;
#endif

/** Defined in `buttons_intern.hh`. */
typedef struct SpaceProperties_Runtime SpaceProperties_Runtime;

#ifdef __cplusplus
namespace blender::ed::space_node {
struct SpaceNode_Runtime;
}  // namespace blender::ed::space_node
using SpaceNode_Runtime = blender::ed::space_node::SpaceNode_Runtime;

namespace blender::ed::outliner {
struct SpaceOutliner_Runtime;
}  // namespace blender::ed::outliner
using SpaceOutliner_Runtime = blender::ed::outliner::SpaceOutliner_Runtime;

namespace blender::ed::seq {
struct SpaceSeq_Runtime;
}  // namespace blender::ed::seq
using SpaceSeq_Runtime = blender::ed::seq::SpaceSeq_Runtime;

namespace blender::ed::text {
struct SpaceText_Runtime;
}  // namespace blender::ed::text
using SpaceText_Runtime = blender::ed::text::SpaceText_Runtime;

namespace blender::ed::spreadsheet {
struct SpaceSpreadsheet_Runtime;
}  // namespace blender::ed::spreadsheet
using SpaceSpreadsheet_Runtime = blender::ed::spreadsheet::SpaceSpreadsheet_Runtime;
#else
typedef struct SpaceNode_Runtime SpaceNode_Runtime;
typedef struct SpaceOutliner_Runtime SpaceOutliner_Runtime;
typedef struct SpaceSeq_Runtime SpaceSeq_Runtime;
typedef struct SpaceText_Runtime SpaceText_Runtime;
typedef struct SpaceSpreadsheet_Runtime SpaceSpreadsheet_Runtime;
#endif

/** Defined in `file_intern.hh`. */
typedef struct SpaceFile_Runtime SpaceFile_Runtime;

/* -------------------------------------------------------------------- */
/** \name SpaceLink (Base)
 * \{ */

/**
 * The base structure all the other spaces
 * are derived (implicitly) from. Would be
 * good to make this explicit.
 */
typedef struct SpaceLink {
  struct SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
} SpaceLink;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Space Info
 * \{ */

/** Info Header. */
typedef struct SpaceInfo {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  char rpt_mask = 0;
  char _pad[7] = {};
} SpaceInfo;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Properties Editor
 * \{ */

/** Properties Editor. */
typedef struct SpaceProperties {
  DNA_DEFINE_CXX_METHODS(SpaceProperties)

  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;

  /* For different kinds of property editors (exposed in the space type selector). */
  short space_subtype = 0;

  /** Context tabs. */
  short mainb = 0, mainbo = 0, mainbuser = 0;
  /** Preview is signal to refresh. */
  short preview = 0;
  char _pad[4] = {};
  char flag = 0;

  /* eSpaceButtons_OutlinerSync */
  char outliner_sync = 0;

  /** Runtime. */
  void *path = nullptr;
  /** Runtime. */
  int pathflag = 0, dataicon = 0;
  ID *pinid = nullptr;

  void *texuser = nullptr;

  /* Doesn't necessarily need to be a pointer, but runtime structs are still written to files. */
  struct SpaceProperties_Runtime *runtime = nullptr;
} SpaceProperties;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Outliner
 * \{ */

/** Outliner */
typedef struct SpaceOutliner {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;

  ListBase tree = {nullptr, nullptr};

  /**
   * Treestore is an ordered list of TreeStoreElem's from outliner tree;
   * Note that treestore may contain duplicate elements if element
   * is used multiple times in outliner tree (e. g. linked objects)
   * Also note that BLI_mempool can not be read/written in DNA directly,
   * therefore `readfile.cc` / `writefile.cc` linearize treestore into #TreeStore structure.
   */
  struct BLI_mempool *treestore = nullptr;

  char search_string[64] = "";

  short flag = 0;
  short outlinevis = 0;
  short lib_override_view_mode = 0;
  short storeflag = 0;
  char search_flags = 0;
  char _pad[6] = {};

  /** Selection syncing flag (#WM_OUTLINER_SYNC_SELECT_FROM_OBJECT and similar flags). */
  char sync_select_dirty = 0;

  int filter = 0;
  char filter_state = 0;
  char show_restrict_flags = 0;
  short filter_id_type = 0;

  SpaceOutliner_Runtime *runtime = nullptr;
} SpaceOutliner;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Graph Editor
 * \{ */

typedef struct SpaceGraph_Runtime {
  /** #eGraphEdit_Runtime_Flag */
  char flag = 0;
  char _pad[7] = {};
  /** Sampled snapshots of F-Curves used as in-session guides */
  ListBase ghost_curves = {nullptr, nullptr};
} SpaceGraph_Runtime;

/** 'Graph' Editor (formerly known as the IPO Editor). */
typedef struct SpaceGraph {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;

  /** Settings for filtering animation data
   * \note we use a pointer due to code-linking issues. */
  struct bDopeSheet *ads = nullptr;

  /** Mode for the Graph editor (eGraphEdit_Mode). */
  short mode = 0;
  /* Snapping now lives on the Scene. */
  short autosnap DNA_DEPRECATED = 0;
  /** Settings for Graph editor (eGraphEdit_Flag). */
  int flag = 0;

  /** Time value for cursor (when in drivers mode; animation uses current frame). */
  float cursorTime = 0;
  /** Cursor value (y-value, x-value is current frame). */
  float cursorVal = 0;
  /** Pivot point for transforms. */
  int around = 0;
  char _pad[4] = {};

  SpaceGraph_Runtime runtime;
} SpaceGraph;

/** \} */

/* -------------------------------------------------------------------- */
/** \name NLA Editor
 * \{ */

/** NLA Editor */
typedef struct SpaceNla {
  struct SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /* Snapping now lives on the Scene. */
  short autosnap DNA_DEPRECATED = 0;
  short flag = 0;
  char _pad[4] = {};

  struct bDopeSheet *ads = nullptr;
  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;
} SpaceNla;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sequence Editor
 * \{ */

typedef struct SequencerPreviewOverlay {
  int flag = 0;
  char _pad0[4] = {};
} SequencerPreviewOverlay;

typedef struct SequencerTimelineOverlay {
  int flag = 0;
  char _pad0[4] = {};
} SequencerTimelineOverlay;

typedef struct SequencerCacheOverlay {
  int flag = 0;
  char _pad0[4] = {};
} SequencerCacheOverlay;

/** Sequencer. */
typedef struct SpaceSeq {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;

  /** Deprecated: offset for drawing the image preview. */
  float xof DNA_DEPRECATED = 0, yof DNA_DEPRECATED = 0;
  /** Weird name for the sequencer subtype (seq, image, luma... etc). */
  short mainb = 0;
  /** ESpaceSeq_Proxy_RenderSize. */
  short render_size = 0;
  short chanshown = 0;
  short zebra = 0;
  int flag = 0;
  /** Deprecated, handled by View2D now. */
  float zoom DNA_DEPRECATED = 0;
  /** See SEQ_VIEW_* below. */
  char view = 0;
  char overlay_frame_type = 0;
  /** Overlay an image of the editing on below the strips. */
  char draw_flag = 0;
  char gizmo_flag = 0;
  char _pad[4] = {};

  /** 2D cursor for transform. */
  float cursor[2] = {};

  /** Grease-pencil data. */
  struct bGPdata *gpd = nullptr;

  struct SequencerPreviewOverlay preview_overlay;
  struct SequencerTimelineOverlay timeline_overlay;
  struct SequencerCacheOverlay cache_overlay;

  /** Multi-view current eye - for internal use. */
  char multiview_eye = 0;
  char _pad2[7] = {};

  SpaceSeq_Runtime *runtime = nullptr;
} SpaceSeq;

typedef struct MaskSpaceInfo {
  /* **** mask editing **** */
  struct Mask *mask = nullptr;
  /* draw options */
  char draw_flag = MASK_DRAWFLAG_SPLINE;
  char draw_type = MASK_DT_OUTLINE;
  char overlay_mode = MASK_OVERLAY_ALPHACHANNEL;
  char _pad3[1] = {};
  float blend_factor = 0.7f;
} MaskSpaceInfo;

/** \} */

/* -------------------------------------------------------------------- */
/** \name File Selector
 * \{ */

/** Config and Input for File Selector. */
typedef struct FileSelectParams {
  /** Title, also used for the text of the execute button. */
  char title[96] = "";
  /**
   * Directory, FILE_MAX_LIBEXTRA, 1024 + 66, this is for extreme case when 1023 length path
   * needs to be linked in, where foo.blend/Armature need adding
   */
  char dir[1090] = "";
  char file[256] = "";

  char renamefile[256] = "";
  short rename_flag = 0;
  char _pad[4] = {};
  /** An ID that was just renamed. Used to identify a renamed asset file over re-reads, similar to
   * `renamefile` but for local IDs (takes precedence). Don't keep this stored across handlers!
   * Would break on undo. */
  const ID *rename_id = nullptr;
  void *_pad3 = nullptr;

  /** List of file-types to filter (#FILE_MAXFILE). */
  char filter_glob[256] = "";

  /** Text items name must match to be shown. */
  char filter_search[64] = "";
  /** Same as filter, but for ID types (aka library groups). */
  uint64_t filter_id = 0;

  /** Active file used for keyboard navigation. */
  int active_file = 0;
  /** File under cursor. */
  int highlight_file = 0;
  int sel_first = 0;
  int sel_last = 0;
  unsigned short thumbnail_size = 0;
  char _pad1[2] = {};

  /* short */
  /** XXX: for now store type here, should be moved to the operator. */
  short type = 0; /* eFileSelectType */
  /** Settings for filter, hiding dots files. */
  short flag = 0;
  /** Sort order. */
  short sort = 0;
  /** Display mode flag. */
  short display = 0;
  /** Details toggles (file size, creation date, etc.) */
  char details_flags = 0;
  char _pad2[3] = {};

  /** Filter when (flags & FILE_FILTER) is true. */
  int filter = 0;

  /** Max number of levels in directory tree to show at once, 0 to disable recursion. */
  short recursion_level = 0;

  char _pad4[2] = {};
} FileSelectParams;

/**
 * File selection parameters for asset browsing mode, with #FileSelectParams as base.
 */
typedef struct FileAssetSelectParams {
  FileSelectParams base_params;

  AssetLibraryReference asset_library_ref;
  short asset_catalog_visibility = 0; /* eFileSel_Params_AssetCatalogVisibility */
  char _pad[6] = {};
  /** If #asset_catalog_visibility is #FILE_SHOW_ASSETS_FROM_CATALOG, this sets the ID of the
   * catalog to show. */
  bUUID catalog_id;

  short import_method = 0; /* eFileAssetImportMethod */
  char _pad2[6] = {};
} FileAssetSelectParams;

/**
 * A wrapper to store previous and next folder lists (#FolderList) for a specific browse mode
 * (#eFileBrowse_Mode).
 */
typedef struct FileFolderHistory {
  struct FileFolderLists *next = nullptr, *prev = nullptr;

  /** The browse mode this prev/next folder-lists are created for. */
  char browse_mode = 0; /* eFileBrowse_Mode */
  char _pad[7] = {};

  /** Holds the list of previous directories to show. */
  ListBase folders_prev = {nullptr, nullptr};
  /** Holds the list of next directories (pushed from previous) to show. */
  ListBase folders_next = {nullptr, nullptr};
} FileFolderHistory;

/** File Browser. */
typedef struct SpaceFile {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Is this a File Browser or an Asset Browser? */
  char browse_mode = 0; /* eFileBrowse_Mode */
  char _pad1[1] = {};

  short tags = 0;

  int scroll_offset = 0;

  /** Config and input for file select. One for each browse-mode, to keep them independent. */
  FileSelectParams *params = nullptr;
  FileAssetSelectParams *asset_params = nullptr;

  void *_pad2 = nullptr;

  /**
   * Holds the list of files to show.
   * Currently recreated when browse-mode changes. Could be per browse-mode to avoid refreshes.
   */
  struct FileList *files = nullptr;

  /**
   * Holds the list of previous directories to show. Owned by `folder_histories` below.
   */
  ListBase *folders_prev = nullptr;
  /**
   * Holds the list of next directories (pushed from previous) to show. Owned by
   * `folder_histories` below.
   */
  ListBase *folders_next = nullptr;

  /**
   * This actually owns the prev/next folder-lists above. On browse-mode change, the lists of the
   * new mode get assigned to the above.
   */
  ListBase folder_histories = {nullptr, nullptr}; /* FileFolderHistory */

  /**
   * The operator that is invoking file-select `op->exec()` will be called on the 'Load' button.
   * if operator provides op->cancel(), then this will be invoked on the cancel button.
   */
  struct wmOperator *op = nullptr;

  struct wmTimer *smoothscroll_timer = nullptr;
  struct wmTimer *previews_timer = nullptr;

  struct FileLayout *layout = nullptr;

  short recentnr = 0, bookmarknr = 0;
  short systemnr = 0, system_bookmarknr = 0;

  SpaceFile_Runtime *runtime = nullptr;
} SpaceFile;

/* ***** Related to file browser, but never saved in DNA, only here to help with RNA. ***** */

#
#
typedef struct FileDirEntry {
  struct FileDirEntry *next = nullptr, *prev = nullptr;

  uint32_t uid = 0; /* FileUID */
  /* Name needs freeing if FILE_ENTRY_NAME_FREE is set. Otherwise this is a direct pointer to a
   * name buffer. */
  const char *name = nullptr;

  uint64_t size = 0;
  int64_t time = 0;

  struct {
    /* Temp caching of UI-generated strings. */
    char size_str[16] = "";
    char datetime_str[16 + 8];
  } draw_data;

  /** #eFileSel_File_Types. */
  int typeflag = 0;
  /** ID type, in case typeflag has FILE_TYPE_BLENDERLIB set. */
  int blentype = 0;

  /* Path to item that is relative to current folder root. To get the full path, use
   * #filelist_file_get_full_path() */
  char *relpath = nullptr;
  /** Optional argument for shortcuts, aliases etc. */
  char *redirection_path = nullptr;

  /** When showing local IDs (FILE_MAIN, FILE_MAIN_ASSET), ID this file represents. Note comment
   * for FileListInternEntry.local_data, the same applies here! */
  ID *id = nullptr;
  /** If this file represents an asset, its asset data is here. Note that we may show assets of
   * external files in which case this is set but not the id above.
   * Note comment for FileListInternEntry.local_data, the same applies here! */
  AssetRepresentationHandle *asset = nullptr;

  /* The icon_id for the preview image. */
  int preview_icon_id = 0;

  short flags = 0;
  /* eFileAttributes defined in BLI_fileops.h */
  int attributes = 0;
} FileDirEntry;

/**
 * Array of directory entries.
 *
 * Stores the total number of available entries, the number of visible (filtered) entries, and a
 * subset of those in 'entries' ListBase, from idx_start (included) to idx_end (excluded).
 */
#
#
typedef struct FileDirEntryArr {
  ListBase entries = {nullptr, nullptr};
  int entries_num = 0;
  int entries_filtered_num = 0;

  /** FILE_MAX. */
  char root[1024] = "";
} FileDirEntryArr;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Image/UV Editor
 * \{ */

/* Image/UV Editor */

typedef struct SpaceImageOverlay {
  int flag = 0;
  char _pad[4] = {};
} SpaceImageOverlay;

typedef struct SpaceImage {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  struct Image *image = nullptr;
  struct ImageUser iuser;

  /** Histogram waveform and vector-scope. */
  struct Scopes scopes;
  /** Sample line histogram. */
  struct Histogram sample_line_hist;

  /** Grease pencil data. */
  struct bGPdata *gpd = nullptr;

  /** UV editor 2d cursor. */
  float cursor[2] = {};
  /** User defined offset, image is centered. */
  float xof = 0, yof = 0;
  /** User defined zoom level. */
  float zoom = 0;
  /** Storage for offset while render drawing. */
  float centx = 0, centy = 0;

  /** View/paint/mask. */
  char mode = 0;
  /* Storage for sub-space types. */
  char mode_prev = 0;

  char pin = 0;

  char pixel_round_mode = 0;

  char lock = 0;
  /** UV draw type. */
  char dt_uv = 0;
  /** Sticky selection type. */
  char dt_uvstretch = 0;
  char around = 0;

  char gizmo_flag = 0;

  char grid_shape_source = 0;
  char _pad1[6] = {};

  int flag = 0;

  float uv_opacity = 0;

  float stretch_opacity = 0;

  int tile_grid_shape[2] = {};
  /**
   * UV editor custom-grid. Value of `{M,N}` will produce `MxN` grid.
   * Use when `custom_grid_shape == SI_GRID_SHAPE_FIXED`.
   */
  int custom_grid_subdiv[2] = {};

  MaskSpaceInfo mask_info;
  SpaceImageOverlay overlay;
} SpaceImage;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text Editor
 * \{ */

/** Text Editor. */
typedef struct SpaceText {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  struct Text *text = nullptr;

  /** Determines at what line the top of the text is displayed. */
  int top = 0;

  /** Determines the horizontal scroll (in columns). */
  int left = 0;
  char _pad1[4] = {};

  short flags = 0;

  /** User preference, is font_size! */
  short lheight = 0;

  int tabnumber = 0;

  /* Booleans */
  char wordwrap = 0;
  char doplugins = 0;
  char showlinenrs = 0;
  char showsyntax = 0;
  char line_hlight = 0;
  char overwrite = 0;
  /** Run python while editing, evil. */
  char live_edit = 0;
  char _pad2[1] = {};

  /** ST_MAX_FIND_STR. */
  char findstr[256] = "";
  /** ST_MAX_FIND_STR. */
  char replacestr[256] = "";

  /** Column number to show right margin at. */
  short margin_column = 0;
  char _pad3[2] = {};

  /** Keep last. */
  SpaceText_Runtime *runtime = nullptr;
} SpaceText;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Script View (Obsolete)
 * \{ */

/** Script Runtime Data - Obsolete (pre 2.5). */
typedef struct Script {
  ID id;

  void *py_draw = nullptr;
  void *py_event = nullptr;
  void *py_button = nullptr;
  void *py_browsercallback = nullptr;
  void *py_globaldict = nullptr;

  int flags = 0, lastspace = 0;
  /**
   * Store the script file here so we can re-run it on loading blender,
   * if "Enable Scripts" is on
   */
  /** 1024 = FILE_MAX. */
  char scriptname[1024] = "";
  /** 1024 = FILE_MAX. */
  char scriptarg[256] = "";
} Script;
#define SCRIPT_SET_NULL(_script) \
  _script->py_draw = _script->py_event = _script->py_button = _script->py_browsercallback = \
      _script->py_globaldict = nullptr; \
  _script->flags = 0

/** Script View - Obsolete (pre 2.5). */
typedef struct SpaceScript {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  struct Script *script = nullptr;

  short flags = 0, menunr = 0;
  char _pad1[4] = {};

  void *but_refs = nullptr;
} SpaceScript;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Nodes Editor
 * \{ */

typedef struct bNodeTreePath {
  struct bNodeTreePath *next = nullptr, *prev = nullptr;

  struct bNodeTree *nodetree = nullptr;
  /** Base key for nodes in this tree instance. */
  bNodeInstanceKey parent_key;
  char _pad[4] = {};
  /** V2d center point, so node trees can have different offsets in editors. */
  float view_center[2] = {};

  /** MAX_NAME. */
  char node_name[64] = "";
  char display_name[64] = "";
} bNodeTreePath;

typedef struct SpaceNodeOverlay {
  /* eSpaceNodeOverlay_Flag */
  int flag = 0;
  /* eSpaceNodeOverlay_preview_shape */
  int preview_shape = 0;
} SpaceNodeOverlay;

typedef struct SpaceNode {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /** Deprecated, copied to region. */
  View2D v2d DNA_DEPRECATED;

  /** Context, no need to save in file? well... pinning... */
  struct ID *id = nullptr, *from = nullptr;

  short flag = 0;

  /** Direction for offsetting nodes on insertion. */
  char insert_ofs_dir = 0;
  char _pad1 = 0;

  /** Offset for drawing the backdrop. */
  float xof = 0, yof = 0;
  /** Zoom for backdrop. */
  float zoom = 0;

  /**
   * XXX nodetree pointer info is all in the path stack now,
   * remove later on and use bNodeTreePath instead.
   * For now these variables are set when pushing/popping
   * from path stack, to avoid having to update all the functions and operators.
   * Can be done when design is accepted and everything is properly tested.
   */
  ListBase treepath = {nullptr, nullptr};

  /* The tree farthest down in the group hierarchy. */
  struct bNodeTree *edittree = nullptr;

  struct bNodeTree *nodetree = nullptr;

  /* tree type for the current node tree */
  char tree_idname[64] = "";
  /** Same as #bNodeTree::type (deprecated). */
  int treetype DNA_DEPRECATED = 0;

  /** Texture-from object, world or brush (#eSpaceNode_TexFrom). */
  short texfrom = 0;
  /** Shader from object or world (#eSpaceNode_ShaderFrom). */
  char shaderfrom = 0;
  /**
   * Whether to edit any geometry node group, or follow the active modifier context.
   * #SpaceNodeGeometryNodesType.
   */
  char geometry_nodes_type = 0;

  /**
   * Used as the editor's top-level node group for #SNODE_GEOMETRY_TOOL. This is stored in the
   * node editor because it isn't part of the context otherwise, and it isn't meant to be set
   * separately from the editor's regular node group.
   */
  struct bNodeTree *geometry_nodes_tool_tree = nullptr;

  /** Grease-pencil data. */
  struct bGPdata *gpd = nullptr;

  SpaceNodeOverlay overlay;

  SpaceNode_Runtime *runtime = nullptr;
} SpaceNode;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Console
 * \{ */

/** Console content. */
typedef struct ConsoleLine {
  struct ConsoleLine *next = nullptr, *prev = nullptr;

  /* Keep these 3 vars so as to share free, realloc functions. */
  /** Allocated length. */
  int len_alloc = 0;
  /** Real length: `strlen()`. */
  int len = 0;
  char *line = nullptr;

  int cursor = 0;
  /** Only for use when in the 'scrollback' listbase. */
  int type = 0;
} ConsoleLine;

/** Console View. */
typedef struct SpaceConsole {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /* Space variables. */

  /** ConsoleLine; output. */
  ListBase scrollback = {nullptr, nullptr};
  /** ConsoleLine; command history, current edited line is the first. */
  ListBase history = {nullptr, nullptr};
  char prompt[256] = "";
  /** Multiple consoles are possible, not just python. */
  char language[32] = "";

  int lheight = 0;

  /** Index into history of most recent up/down arrow keys. */
  int history_index = 0;

  /** Selection offset in bytes. */
  int sel_start = 0;
  int sel_end = 0;
} SpaceConsole;

/** \} */

/* -------------------------------------------------------------------- */
/** \name User Preferences
 * \{ */

typedef struct SpaceUserPref {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  char _pad1[7] = {};
  char filter_type = 0;
  /** Search term for filtering in the UI. */
  char filter[64] = "";
} SpaceUserPref;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Motion Tracking
 * \{ */

/** Clip Editor. */
typedef struct SpaceClip {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = SPACE_CLIP;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  char gizmo_flag = 0;
  char _pad1[3] = {};

  /** User defined offset, image is centered. */
  float xof = 0, yof = 0;
  /** User defined offset from locked position. */
  float xlockof = 0, ylockof = 0;
  /** User defined zoom level. */
  float zoom = 1.0f;

  /** User of clip. */
  struct MovieClipUser user;
  /** Clip data. */
  struct MovieClip *clip = nullptr;
  /** Different scoped displayed in space panels. */
  struct MovieClipScopes scopes;

  /** Flags. */
  int flag = SC_SHOW_MARKER_PATTERN | SC_SHOW_TRACK_PATH | SC_SHOW_GRAPH_TRACKS_MOTION |
             SC_SHOW_GRAPH_FRAMES | SC_SHOW_ANNOTATION;
  /** Editor mode (editing context being displayed). */
  short mode = SC_MODE_TRACKING;
  /** Type of the clip editor view. */
  short view = SC_VIEW_CLIP;

  /** Length of displaying path, in frames. */
  int path_length = 20;

  /* current stabilization data */
  /** Pre-composed stabilization data. */
  float loc[2] = {0, 0}, scale = 0, angle = 0;
  char _pad[4] = {};
  /**
   * Current stabilization matrix and the same matrix in unified space,
   * defined when drawing and used for mouse position calculation.
   */
  float stabmat[4][4] = _DNA_DEFAULT_UNIT_M4, unistabmat[4][4] = _DNA_DEFAULT_UNIT_M4;

  /** Movie postprocessing. */
  int postproc_flag = 0;

  /* grease pencil */
  short gpencil_src = SC_GPENCIL_SRC_CLIP;
  char _pad2[2] = {};

  /** Pivot point for transforms. */
  int around = V3D_AROUND_CENTER_MEDIAN;
  char _pad4[4] = {};

  /** Mask editor 2d cursor. */
  float cursor[2] = {0, 0};

  MaskSpaceInfo mask_info;
} SpaceClip;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Top Bar
 * \{ */

typedef struct SpaceTopBar {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */
} SpaceTopBar;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Status Bar
 * \{ */

typedef struct SpaceStatusBar {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */
} SpaceStatusBar;

/** \} */

/* -------------------------------------------------------------------- */
/** \name Spreadsheet
 * \{ */

typedef struct SpreadsheetColumnID {
  char *name = nullptr;
} SpreadsheetColumnID;

typedef struct SpreadsheetColumn {
  struct SpreadsheetColumn *next = nullptr, *prev = nullptr;
  /**
   * Identifies the data in the column.
   * This is a pointer instead of a struct to make it easier if we want to "subclass"
   * #SpreadsheetColumnID in the future for different kinds of ids.
   */
  SpreadsheetColumnID *id = nullptr;

  /**
   * An indicator of the type of values in the column, set at runtime.
   * #eSpreadsheetColumnValueType.
   */
  uint8_t data_type = 0;
  char _pad0[7] = {};

  /**
   * The final column name generated by the data source, also just
   * cached at runtime when the data source columns are generated.
   */
  char *display_name = nullptr;
} SpreadsheetColumn;

typedef struct SpreadsheetInstanceID {
  int reference_index = 0;
} SpreadsheetInstanceID;

typedef struct SpaceSpreadsheet {
  SpaceLink *next = nullptr, *prev = nullptr;
  /** Storage of regions for inactive spaces. */
  ListBase regionbase = {nullptr, nullptr};
  char spacetype = 0;
  char link_flag = 0;
  char _pad0[6] = {};
  /* End 'SpaceLink' header. */

  /* List of #SpreadsheetColumn. */
  ListBase columns = {nullptr, nullptr};

  /* SpreadsheetRowFilter. */
  ListBase row_filters = {nullptr, nullptr};

  /**
   * Context that is currently displayed in the editor. This is usually a either a single object
   * (in original/evaluated mode) or path to a viewer node. This is retrieved from the workspace
   * but can be pinned so that it stays constant even when the active node changes.
   */
  ViewerPath viewer_path;

  /**
   * The "path" to the currently active instance reference. This is needed when viewing nested
   * instances.
   */
  SpreadsheetInstanceID *instance_ids = nullptr;
  int instance_ids_num = 0;

  /* eSpaceSpreadsheet_FilterFlag. */
  uint8_t filter_flag = 0;

  /* #GeometryComponent::Type. */
  uint8_t geometry_component_type = 0;
  /* #AttrDomain. */
  uint8_t attribute_domain = 0;
  /* eSpaceSpreadsheet_ObjectEvalState. */
  uint8_t object_eval_state = 0;
  /* Active grease pencil layer index for grease pencil component. */
  int active_layer_index = 0;

  /* eSpaceSpreadsheet_Flag. */
  uint32_t flag = 0;

  SpaceSpreadsheet_Runtime *runtime = nullptr;
} SpaceSpreadsheet;

typedef struct SpreadsheetRowFilter {
  struct SpreadsheetRowFilter *next = nullptr, *prev = nullptr;

  char column_name[64] = ""; /* MAX_NAME. */

  /* eSpreadsheetFilterOperation. */
  uint8_t operation = 0;
  /* eSpaceSpreadsheet_RowFilterFlag. */
  uint8_t flag = 0;

  char _pad0[2] = {};

  int value_int = 0;
  int value_int2[2] = {};
  char *value_string = nullptr;
  float value_float = 0;
  float threshold = 0;
  float value_float2[2] = {};
  float value_float3[3] = {};
  float value_color[4] = {};
  char _pad1[4] = {};
} SpreadsheetRowFilter;

/** \} */
