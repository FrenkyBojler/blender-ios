
##########
Extensions
##########

Extensions Source Code Overview
===============================

Add-on: Blender Modules
-----------------------

- ``__init__.py``
  Add-on containing, preferences, ``bpy.app.handlers`` that respond to adding/removing repositories.

- ``bl_extension_ui.py``
  Defines the extensions UI, select between add-ons, themes, tag-filtering.

- ``bl_extension_ops.py``
  Defines extension operators, this is the main entry point for extension logic (besides notifications).

  This module defines a mechanism for a modal operator to run commands as sub-processes
  monitoring their progress (via STDOUT).
  Actions such a as downloading, installing, updating are supported.

  There are also some operators for the UI (changing tags, allowing online access).

- ``bl_extension_notify.py``

  This is a module that checks for updates,
  unlike the operator (which can also check for updates),
  this is intended to run in the background without using an operator,
  the status bar is refreshed if/when updates are found.

- ``bl_extension_cli.py``

  The command line interface to support: ``blender -c extension ...``

  Some commands operate on Blender's preferences (for adding/removing repositories),
  other commands such as building packages are forwarded to ``cli/blender_ext.py``.

Add-on: Other Scripts
---------------------

- ``bl_extension_utils.py``

  This module contains various utilities,

  Note that use of ``bpy`` is intentionally avoided here,
  state from preferences or operators is passed in.

  - Generic shared utility functions.

  - Command line sub-process supervisor (``CommandBatch``)
    used by Blender operators.

  - A view on the repositories JSON/TOML data what abstracts the file IO (``RepoCacheStore``).

  - A locking context to prevent multiple Blender instances operating on the same repository at once.

- ``cli/blender_ext.py``
  This command is responsible for operations on the repository,
  primarily downloading & installing extensions.

  It also contains functions to build & validate packages & create a static repository.

  This script typically runs as an external process
  (called by ``bl_extension_utils.py`` or in some cases ``bl_extension_cli.py``).

  Inter process communication (IPC) is used so Blender's UI can show the status of each command.

- ``extensions_map_from_legacy_addons``

  This is more of a data file used so users with legacy add-ons can update them to extensions.

Other Modules
-------------

Some functionality is included elsewhere as it relates to how extensions are loaded by Blender.

*Paths are relative to Blender's source tree.*

``./scripts/modules/_bpy_internal/extensions/junction_module.py``
   This stand-alone module allows extensions to appear as if they are all loaded
   from a single *package* independent of their file-system location.

   This is done so extensions don't pollute the module name-space
   (avoiding naming collisions with ``https://pypi.org``).

   So each extension's add-on ID resembles: ``bl_ext.{repository_id}.{extension_id}``.

``./scripts/modules/_bpy_internal/extensions/wheel_manager.py``
   Extensions may include Python modules as wheels,
   these need to be extracted into an extension-local site-packages, e.g.
   ``~/.config/blender/X.X/extensions/.local/lib/python3.XX/site-packages/``.

   Once extensions have been installed a list of wheels is passed in to the main "apply_action" function
   which will install/uninstall wheels as needed.

   Unfortunately there is no special handling for version conflicts.
   When different versions of the same wheel are found, the latest version is installed.
   This will break any extensions depending on the old version of a wheel.

``./scripts/modules/_bpy_internal/extensions/stale_file_manager.py``
   On MS-Windows it's common that files are locked and can't be deleted
   (any DLL's loaded into memory),
   although it can also happen if other processes are scanning the file system.

   In this case, the file is marked as stale and queued for removal when Blender next starts.

   Unfortunately **upgrading** extensions that use DLL's on MS-Windows isn't
   reliable because it is necessary to remove then re-create the extension.
   This is an area that could use further development it may be necessary to
   support installing on restart.

``./scripts/modules/_bpy_internal/extensions/{tags,permissions}.py``
   These are definition lists used when building packages,
   they are ``https://extensions.blender.org`` specific.


C++ Sources
-----------

``./source/blender/makesdna/DNA_userdef_types.h``
   The repository definition (``bUserExtensionRepo``).

``./source/blender/editors/space_userpref/userpref_ops.cc``
   Operators for adding/removing repositories as well as dropping URL's to initiate installation.

``./source/blender/python/intern/bpy_app_handlers.cc``
   Handlers for extensions ``bpy.app.handlers._extension_repos_*``.

   Unfortunately these handlers were needed as a way for Python to hook into lower level code paths,
   so it's possible (for example) to refresh the extensions from an RNA update function
   (``rna_userdef.cc`` and the operators in some cases).

``wmWindowManager::extensions_updates`` & ``extensions_blocked``
   Status bar drawing uses these values set by ``bl_extension_notify.py``.


Functionality Described
=======================

This section describes how functionality has been implemented.


Extension Pre-Flight Compatibility Check
----------------------------------------

Since extensions may be from a shared system directory or imported from an older installation
it's necessary to ensure the extension is compatible with Blender on startup.

An extension may be incompatible for various reasons,

- Unsupported Blender version.
- Unsupported platform.
- Binary incompatibility (from it's wheels).
- The extension may also have been block-listed.

Since this runs on every startup, expensive checks are avoided if at all possible.

In ``./scripts/modules/addon_utils.py`` the private function ``_initialize_extensions_compat_ensure_up_to_date``
is responsible for ensuring extensions are compatible before loading.

- On startup run: ``_initialize_extensions_repos_once`` which sets up repositories and handlers.
- If the "compatibility cache" doesn't exist it is created (each extension's TOML file is inspected for compatibility).
  A dictionary of incompatible extensions is stored in the compatibility cache which is checked
  whoever ``addon_utils.enable(..)`` is used to enable an extension.
- If the "compatibility cache" exists it is validated by each extensions TOML modification-time & size,
  re-generating upon any changes.
- The compatibility data stores the reason the extension being disabled,
  this is reported if the user attempts to enable it.


The details of the compatibility cache are document in ``addon_utils``,
it's a simple format that stores the Blender version & a magic number that can be bumped at any time,
changes to these files cause the cache to be re-generated.


Dropping a URL
--------------

Dropping extensions is handled the same way as dropping images or other strings,
using Blender's drop-boxes.

There are two drop-boxes used one for file-paths another for URL's.
Both check the path contains a ``.zip`` extension,
where the URL logic needs to strip any parameters which may be part of the URL.

The drop action runs the operator ``extensions.package_install`` (from ``bl_extension_ops.py``)
which checks if the ``url`` property has been set.
If so, the code-path for dropping a URL is activated.

Once drop is activated:

- A URL is scanned for blender version range & platform compatibility
  to prevent downloading & attempting to install extensions which aren't compatible.

- A file-path is considered "local" so it's manifest is inspected to check it's compatible.

Other checks are performed to ensure the repository exists locally.
If the extension isn't found to be incompatible, the user may install it.

Unfortunately chaining popups together (setup wizard) is not well supported in Blender.
Causing some fairly bad worst-case scenarios when dropping a URL which isn't part of a known repository.


Internal Details
================

Extension Format
----------------

Extensions are intended to be created with the ``blender -c extension build``
command which creates a ZIP file and performs some checks to catch errors early.

The ZIP file must contain a ``blender_manifest.toml`` (which may be in a directory),
as well as files for a Python package for add-ons or an XML for themes.


Repositories
------------

Information about repositories are stored in user preferences,
The main values are a unique name, module path & optionally a remote URL.

When the URL is set this is used for synchronizing updates.

Synchronizing the repository is simply downloading the JSON listing.

Repositories may also be system repositories (assumed to be read-only) or local
where the user manages the files.

Once extensions have been installed their TOML files are compared with the repository to check for updates.


Inter Process Communication (IPC)
---------------------------------

- Most long running operations are performed by the stand-alone script ``cli/blender_ext.py``.
- Its state is passed in via command line arguments.
- This can be configured to only output JSON messages to the STDOUT which Blender parses and uses
  to send feedback to the user.
- Input is limited to the request to cancel
  (if the user cancels the operator or presses Control-C on the command line).
- Internally actions are split up in small steps to avoid "hanging" once the user has requested to exit.

Most IPC is handled by ``bl_extension_utils.CommandBatch`` which can run multiple commands,
a common case is running multiple updates at once.

The caller can use methods on the CommandBatch to access the status and report any problems.


Tooling
=======

The tests are not yet integrated into CTest.

Some of the tests require the ``wheel`` package to be installed locally via ``pip``,
used for generating ``*.whl`` files.

Tests can be run via the Make file using the local Python::

   make -C scripts/addons_core/bl_pkg test

Run the ``help`` target for a list of convenience targets to run checkers & tests.
