/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bli
 * WIN32-POSIX compatibility layer, MS-Windows-specific functions.
 */

#ifdef WIN32

#  include <conio.h>
#  include <shlwapi.h>
#  include <stdio.h>
#  include <stdlib.h>
#  define COBJMACROS /* Remove this when converting to C++ */
#  include <dxgi.h>
#  include <mlang.h>
#  include <vector>
#  include <wrl.h>

#  include "MEM_guardedalloc.h"

#  define WIN32_SKIP_HKEY_PROTECTION /* Need to use HKEY. */
#  include "BLI_fileops.h"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "BLI_utildefines.h"
#  include "BLI_winstuff.h"

#  include "utf_winfunc.hh"
#  include "utfconv.hh"

/* FILE_MAXDIR + FILE_MAXFILE */

int BLI_windows_get_executable_dir(char r_dirpath[/*FILE_MAXDIR*/])
{
  char filepath[FILE_MAX];
  char dir[FILE_MAX];
  int a;
  /* Change to UTF support. */
  GetModuleFileName(nullptr, filepath, sizeof(filepath));
  BLI_path_split_dir_part(filepath, dir, sizeof(dir)); /* shouldn't be relative */
  a = strlen(dir);
  if (dir[a - 1] == '\\') {
    dir[a - 1] = 0;
  }

  BLI_strncpy(r_dirpath, dir, FILE_MAXDIR);

  return 1;
}

bool BLI_windows_is_store_install(void)
{
  char install_dir[FILE_MAXDIR];
  BLI_windows_get_executable_dir(install_dir);
  return (BLI_strcasestr(install_dir, "\\WindowsApps\\") != nullptr);
}

static void registry_error(HKEY root, const char *message)
{
  if (root) {
    RegCloseKey(root);
  }
  fprintf(stderr, "%s\n", message);
}

static bool open_registry_hive(bool all_users, HKEY *r_root)
{
  if (RegOpenKeyEx(all_users ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER,
                   "Software\\Classes",
                   0,
                   KEY_ALL_ACCESS,
                   r_root) != ERROR_SUCCESS)
  {
    registry_error(*r_root, "Unable to open the registry with the required permissions");
    return false;
  }
  return true;
}

static bool register_blender_prog_id(const char *prog_id,
                                     const char *executable,
                                     const char *friendly_name,
                                     bool all_users)
{
  LONG lresult;
  HKEY root = 0;
  HKEY hkey_progid = 0;
  char buffer[256];
  DWORD dwd = 0;

  if (!open_registry_hive(all_users, &root)) {
    return false;
  }

  lresult = RegCreateKeyEx(root,
                           prog_id,
                           0,
                           nullptr,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           nullptr,
                           &hkey_progid,
                           &dwd);

  if (lresult == ERROR_SUCCESS) {
    lresult = RegSetValueEx(
        hkey_progid, nullptr, 0, REG_SZ, (BYTE *)friendly_name, strlen(friendly_name) + 1);
  }
  if (lresult == ERROR_SUCCESS) {
    lresult = RegSetValueEx(
        hkey_progid, "AppUserModelId", 0, REG_SZ, (BYTE *)prog_id, strlen(prog_id) + 1);
  }
  if (lresult != ERROR_SUCCESS) {
    registry_error(root, "Unable to register Blender App Id");
    return false;
  }

  SNPRINTF(buffer, "%s\\shell\\open", prog_id);
  lresult = RegCreateKeyEx(root,
                           buffer,
                           0,
                           nullptr,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           nullptr,
                           &hkey_progid,
                           &dwd);

  lresult = RegSetValueEx(
      hkey_progid, "FriendlyAppName", 0, REG_SZ, (BYTE *)friendly_name, strlen(friendly_name) + 1);

  SNPRINTF(buffer, "%s\\shell\\open\\command", prog_id);

  lresult = RegCreateKeyEx(root,
                           buffer,
                           0,
                           nullptr,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           nullptr,
                           &hkey_progid,
                           &dwd);

  if (lresult == ERROR_SUCCESS) {
    SNPRINTF(buffer, "\"%s\" \"%%1\"", executable);
    lresult = RegSetValueEx(hkey_progid, nullptr, 0, REG_SZ, (BYTE *)buffer, strlen(buffer) + 1);
    RegCloseKey(hkey_progid);
  }
  if (lresult != ERROR_SUCCESS) {
    registry_error(root, "Unable to register Blender App Id");
    return false;
  }

  SNPRINTF(buffer, "%s\\DefaultIcon", prog_id);
  lresult = RegCreateKeyEx(root,
                           buffer,
                           0,
                           nullptr,
                           REG_OPTION_NON_VOLATILE,
                           KEY_ALL_ACCESS,
                           nullptr,
                           &hkey_progid,
                           &dwd);

  if (lresult == ERROR_SUCCESS) {
    SNPRINTF(buffer, "\"%s\", 1", executable);
    lresult = RegSetValueEx(hkey_progid, nullptr, 0, REG_SZ, (BYTE *)buffer, strlen(buffer) + 1);
    RegCloseKey(hkey_progid);
  }
  if (lresult != ERROR_SUCCESS) {
    registry_error(root, "Unable to register Blender App Id");
    return false;
  }
  return true;
}

bool BLI_windows_register_blend_extension(const bool all_users)
{
  if (BLI_windows_is_store_install()) {
    fprintf(stderr, "Registration not possible from Microsoft Store installation.");
    return false;
  }

  HKEY root = 0;
  char blender_path[MAX_PATH];
  char *blender_app;
  HKEY hkey = 0;
  LONG lresult;
  DWORD dwd = 0;
  const char *prog_id = BLENDER_WIN_APPID;
  const char *friendly_name = BLENDER_WIN_APPID_FRIENDLY_NAME;

  GetModuleFileName(0, blender_path, sizeof(blender_path));

  /* Prevent overflow when we add -launcher to the executable name. */
  if (strlen(blender_path) > (sizeof(blender_path) - 10)) {
    return false;
  }

  /* Replace the actual app name with the wrapper. */
  blender_app = strstr(blender_path, "blender.exe");
  if (!blender_app) {
    return false;
  }
  strcpy(blender_app, "blender-launcher.exe");

  if (!open_registry_hive(all_users, &root)) {
    return false;
  }

  if (!register_blender_prog_id(prog_id, blender_path, friendly_name, all_users)) {
    registry_error(root, "Unable to register Blend document type");
    return false;
  }

  lresult = RegCreateKeyEx(
      root, ".blend", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &hkey, &dwd);
  if (lresult == ERROR_SUCCESS) {
    /* Set this instance the default. */
    lresult = RegSetValueEx(hkey, nullptr, 0, REG_SZ, (BYTE *)prog_id, strlen(prog_id) + 1);

    if (lresult != ERROR_SUCCESS) {
      registry_error(root, "Unable to register Blend document type");
      RegCloseKey(hkey);
      return false;
    }
    RegCloseKey(hkey);

    lresult = RegCreateKeyEx(root,
                             ".blend\\OpenWithProgids",
                             0,
                             nullptr,
                             REG_OPTION_NON_VOLATILE,
                             KEY_ALL_ACCESS,
                             nullptr,
                             &hkey,
                             &dwd);

    if (lresult != ERROR_SUCCESS) {
      registry_error(root, "Unable to register Blend document type");
      RegCloseKey(hkey);
      return false;
    }
    lresult = RegSetValueEx(hkey, prog_id, 0, REG_NONE, nullptr, 0);
    RegCloseKey(hkey);
  }

  if (lresult != ERROR_SUCCESS) {
    registry_error(root, "Unable to register Blend document type");
    return false;
  }

  if (!BLI_windows_update_pinned_launcher(blender_path)) {
    fprintf(stderr, "Update of pinned launcher failed.");
    return false;
  }

#  ifdef WITH_BLENDER_THUMBNAILER
  {
    char reg_cmd[MAX_PATH * 2];
    char install_dir[FILE_MAXDIR];
    char system_dir[FILE_MAXDIR];
    BLI_windows_get_executable_dir(install_dir);
    GetSystemDirectory(system_dir, sizeof(system_dir));
    const char *thumbnail_handler = "BlendThumb.dll";
    SNPRINTF(reg_cmd, "%s\\regsvr32 /s \"%s\\%s\"", system_dir, install_dir, thumbnail_handler);
    system(reg_cmd);
  }
#  endif

  RegCloseKey(root);
  char message[256];
  SNPRINTF(message,
           "Blend file extension registered for %s.",
           all_users ? "all users" : "the current user");
  printf("%s\n", message);

  return true;
}

bool BLI_windows_unregister_blend_extension(const bool all_users)
{
  if (BLI_windows_is_store_install()) {
    fprintf(stderr, "Unregistration not possible from Microsoft Store installation.");
    return false;
  }

  HKEY root = 0;
  HKEY hkey = 0;
  LONG lresult;

  if (!open_registry_hive(all_users, &root)) {
    return false;
  }

  /* Don't stop on failure. We want to allow unregister after unregister. */

  RegDeleteTree(root, BLENDER_WIN_APPID);

  lresult = RegOpenKeyEx(root, ".blend", 0, KEY_ALL_ACCESS, &hkey);
  if (lresult == ERROR_SUCCESS) {
    char buffer[256] = {0};
    DWORD size = sizeof(buffer);
    lresult = RegGetValueA(hkey, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, &buffer, &size);
    if (lresult == ERROR_SUCCESS && STREQ(buffer, BLENDER_WIN_APPID)) {
      RegSetValueEx(hkey, nullptr, 0, REG_SZ, 0, 0);
    }
  }

#  ifdef WITH_BLENDER_THUMBNAILER
  {
    char reg_cmd[MAX_PATH * 2];
    char install_dir[FILE_MAXDIR];
    char system_dir[FILE_MAXDIR];
    BLI_windows_get_executable_dir(install_dir);
    GetSystemDirectory(system_dir, sizeof(system_dir));
    const char *thumbnail_handler = "BlendThumb.dll";
    SNPRINTF(reg_cmd, "%s\\regsvr32 /u /s \"%s\\%s\"", system_dir, install_dir, thumbnail_handler);
    system(reg_cmd);
  }
#  endif

  lresult = RegOpenKeyEx(hkey, "OpenWithProgids", 0, KEY_ALL_ACCESS, &hkey);
  if (lresult == ERROR_SUCCESS) {
    RegDeleteValue(hkey, BLENDER_WIN_APPID);
  }

  RegCloseKey(root);
  char message[256];
  SNPRINTF(message,
           "Blend file extension unregistered for %s.",
           all_users ? "all users" : "the current user");
  printf("%s\n", message);

  return true;
}

/**
 * Check the registry to see if there is an operation association to a file
 * extension. Extension *should almost always contain a dot like `.txt`,
 * but this does allow querying non - extensions *like "Directory", "Drive",
 * "AllProtocols", etc - anything in Classes with a "shell" branch.
 */
static bool BLI_windows_file_operation_is_registered(const char *extension, const char *operation)
{
  HKEY hKey;
  HRESULT hr = AssocQueryKey(ASSOCF_INIT_IGNOREUNKNOWN,
                             ASSOCKEY_SHELLEXECCLASS,
                             (LPCTSTR)extension,
                             (LPCTSTR)operation,
                             &hKey);
  if (SUCCEEDED(hr)) {
    RegCloseKey(hKey);
    return true;
  }
  return false;
}

bool BLI_windows_external_operation_supported(const char *filepath, const char *operation)
{
  if (STREQ(operation, "open") || STREQ(operation, "properties")) {
    return true;
  }

  if (BLI_is_dir(filepath)) {
    return BLI_windows_file_operation_is_registered("Directory", operation);
  }

  const char *extension = BLI_path_extension(filepath);
  return BLI_windows_file_operation_is_registered(extension, operation);
}

bool BLI_windows_external_operation_execute(const char *filepath, const char *operation)
{
  WCHAR wpath[FILE_MAX];
  if (conv_utf_8_to_16(filepath, wpath, ARRAY_SIZE(wpath)) != 0) {
    return false;
  }

  WCHAR woperation[FILE_MAX];
  if (conv_utf_8_to_16(operation, woperation, ARRAY_SIZE(woperation)) != 0) {
    return false;
  }

  SHELLEXECUTEINFOW shellinfo = {0};
  shellinfo.cbSize = sizeof(SHELLEXECUTEINFO);
  shellinfo.fMask = SEE_MASK_INVOKEIDLIST;
  shellinfo.lpVerb = woperation;
  shellinfo.lpFile = wpath;
  shellinfo.nShow = SW_SHOW;

  return ShellExecuteExW(&shellinfo);
}

bool BLI_windows_execute_self(const char *parameters,
                              const bool wait,
                              const bool elevated,
                              const bool silent)
{
  char blender_path[MAX_PATH];
  GetModuleFileName(0, blender_path, MAX_PATH);

  SHELLEXECUTEINFOA shellinfo = {0};
  shellinfo.cbSize = sizeof(SHELLEXECUTEINFO);
  shellinfo.fMask = wait ? SEE_MASK_NOCLOSEPROCESS : SEE_MASK_DEFAULT;
  shellinfo.hwnd = nullptr;
  shellinfo.lpVerb = elevated ? "runas" : nullptr;
  shellinfo.lpFile = blender_path;
  shellinfo.lpParameters = parameters;
  shellinfo.lpDirectory = nullptr;
  shellinfo.nShow = silent ? SW_HIDE : SW_SHOW;
  shellinfo.hInstApp = nullptr;
  shellinfo.hProcess = 0;

  DWORD exitCode = 0;
  if (!ShellExecuteExA(&shellinfo)) {
    return false;
  }
  if (!wait) {
    return true;
  }

  if (shellinfo.hProcess != 0) {
    WaitForSingleObject(shellinfo.hProcess, INFINITE);
    GetExitCodeProcess(shellinfo.hProcess, &exitCode);
    CloseHandle(shellinfo.hProcess);
    return (exitCode == 0);
  }

  return false;
}

void BLI_windows_get_default_root_dir(char root[4])
{
  char str[MAX_PATH + 1];

  /* the default drive to resolve a directory without a specified drive
   * should be the Windows installation drive, since this was what the OS
   * assumes. */
  if (GetWindowsDirectory(str, MAX_PATH + 1)) {
    root[0] = str[0];
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
  }
  else {
    /* if GetWindowsDirectory fails, something has probably gone wrong,
     * we are trying the blender install dir though */
    if (GetModuleFileName(nullptr, str, MAX_PATH + 1)) {
      printf(
          "Error! Could not get the Windows Directory - "
          "Defaulting to Blender installation Dir!\n");
      root[0] = str[0];
      root[1] = ':';
      root[2] = '\\';
      root[3] = '\0';
    }
    else {
      DWORD tmp;
      int i;
      int rc = 0;
      /* now something has gone really wrong - still trying our best guess */
      printf(
          "Error! Could not get the Windows Directory - "
          "Defaulting to first valid drive! Path might be invalid!\n");
      tmp = GetLogicalDrives();
      for (i = 2; i < 26; i++) {
        if ((tmp >> i) & 1) {
          root[0] = 'a' + i;
          root[1] = ':';
          root[2] = '\\';
          root[3] = '\0';
          if (GetFileAttributes(root) != 0xFFFFFFFF) {
            rc = i;
            break;
          }
        }
      }
      if (0 == rc) {
        printf("ERROR in 'BLI_windows_get_default_root_dir': cannot find a valid drive!\n");
        root[0] = 'C';
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
      }
    }
  }
}

bool BLI_windows_get_directx_driver_version(const wchar_t *deviceSubString,
                                            long long *r_driverVersion)
{
  IDXGIFactory *pFactory = nullptr;
  IDXGIAdapter *pAdapter = nullptr;
  if (CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&pFactory) == S_OK) {
    for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
      LARGE_INTEGER version;
      if (pAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version) == S_OK) {
        DXGI_ADAPTER_DESC desc;
        if (pAdapter->GetDesc(&desc) == S_OK) {
          if (wcsstr(desc.Description, deviceSubString)) {
            *r_driverVersion = version.QuadPart;

            pAdapter->Release();
            pFactory->Release();
            return true;
          }
        }
      }

      pAdapter->Release();
    }
    pFactory->Release();
  }

  return false;
}

bool BLI_windows_is_build_version_greater_or_equal(DWORD majorVersion,
                                                   DWORD minorVersion,
                                                   DWORD buildNumber)
{
  HMODULE hMod = ::GetModuleHandleW(L"ntdll.dll");
  if (hMod == 0) {
    return false;
  }

  typedef NTSTATUS(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
  RtlGetVersionPtr rtl_get_version = (RtlGetVersionPtr)::GetProcAddress(hMod, "RtlGetVersion");
  if (rtl_get_version == nullptr) {
    fprintf(stderr, "BLI_windows_is_build_version_greater_or_equal: RtlGetVersion not found.");
    return false;
  }

  RTL_OSVERSIONINFOW osVersioninfo{};
  osVersioninfo.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOW);
  if (rtl_get_version(&osVersioninfo) != 0) {
    fprintf(stderr, "BLI_windows_is_build_version_greater_or_equal: RtlGetVersion failed.");
    return false;
  }
  if (majorVersion != osVersioninfo.dwMajorVersion) {
    return osVersioninfo.dwMajorVersion > majorVersion;
  }
  if (minorVersion != osVersioninfo.dwMinorVersion) {
    return osVersioninfo.dwMajorVersion > minorVersion;
  }
  return osVersioninfo.dwBuildNumber >= buildNumber;
}

void BLI_windows_process_set_qos(QoSMode qos_mode, QoSPrecedence qos_precedence)
{
  static QoSPrecedence qos_precedence_last = QoSPrecedence::JOB;
  if (int(qos_precedence) < int(qos_precedence_last)) {
    return;
  }

  /* Only supported on Windows build >= 10.0.22000, i.e., Windows 11 21H2:
   * https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ne-processthreadsapi-process_information_class
   */
  if (!BLI_windows_is_build_version_greater_or_equal(10, 0, 22000)) {
    return;
  }

  PROCESS_POWER_THROTTLING_STATE processPowerThrottlingState{};
  processPowerThrottlingState.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  switch (qos_mode) {
    case QoSMode::DEFAULT:
      processPowerThrottlingState.ControlMask = 0;
      processPowerThrottlingState.StateMask = 0;
      break;
    case QoSMode::HIGH:
      processPowerThrottlingState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      processPowerThrottlingState.StateMask = 0;
      break;
    case QoSMode::ECO:
      processPowerThrottlingState.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      processPowerThrottlingState.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      break;
  }
  HANDLE hProcess = GetCurrentProcess();
  if (!SetProcessInformation(hProcess,
                             ProcessPowerThrottling,
                             &processPowerThrottlingState,
                             sizeof(PROCESS_POWER_THROTTLING_STATE)))
  {
    fprintf(
        stderr, "BLI_windows_set_process_qos: SetProcessInformation failed: %d\n", GetLastError());
    return;
  }
  qos_precedence_last = qos_precedence;
}

size_t BLI_windows_read_rsp_fileW(LPCWSTR filename, LPWSTR buffer, size_t buffer_size)
{
  HANDLE hFile = CreateFileW(filename,
                             GENERIC_READ,
                             FILE_SHARE_READ,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (hFile == INVALID_HANDLE_VALUE)
    return 0;

  DWORD fileSize = GetFileSize(hFile, nullptr);
  if (fileSize == INVALID_FILE_SIZE || fileSize == 0) {
    CloseHandle(hFile);
    return 0;
  }
  /* Not using the blender vector class here because we can't use `guardedalloc` allocation here,
   * as it's not yet initialized (it depends on the arguments passed in, which is what we're
   * getting here!). */
  std::vector<BYTE> raw(fileSize);
  DWORD bytesRead = 0;
  if (!ReadFile(hFile, raw.data(), fileSize, &bytesRead, nullptr)) {
    CloseHandle(hFile);
    return 0;
  }
  CloseHandle(hFile);

  Microsoft::WRL::ComPtr<IMultiLanguage2> pMLang;
  HRESULT hr = CoCreateInstance(CLSID_CMultiLanguage,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IMultiLanguage2,
                                reinterpret_cast<void **>(pMLang.GetAddressOf()));
  if (FAILED(hr)) {
    return 0;
  }

  /* Try to detect what kind of encoding is used is in the buffer. */
  DetectEncodingInfo encInfo[1] = {};
  INT nScores = 1;
  UINT codePage =
      CP_UTF8; /* if no detection can be made, just assume UTF8 and hope for the best. */

  /* Inconsistent API DetectInputCodepage takes an int, ConvertStringToUnicode takes an uint, just
   * add the type to the variable name to differentiate between the two. */
  INT srcSize_int = INT(bytesRead);
  hr = pMLang->DetectInputCodepage(
      MLDETECTCP_NONE, 0, reinterpret_cast<CHAR *>(raw.data()), &srcSize_int, encInfo, &nScores);

  if (SUCCEEDED(hr) && nScores > 0) {
    codePage = encInfo[0].nCodePage;
  }

  /* Note: The most obviouos way to do this is just call MultiByteToWideChar here, but that won't
   * be able to deal with UTF 16 BE/LE so an alternative method has been chosen, since we already
   * used mlang for detecting, we may as well keep using it. */

  /* Figure out how big of a buffer we need. */
  UINT dstSize = 0;
  DWORD mode = 0;
  UINT srcSize_uint = UINT(bytesRead);
  hr = pMLang->ConvertStringToUnicode(
      &mode, codePage, reinterpret_cast<CHAR *>(raw.data()), &srcSize_uint, nullptr, &dstSize);
  if (FAILED(hr))
    return 0;

  size_t required = size_t(dstSize) + 1; /* +1 for zero terminator. */
  if (buffer_size == 0) {
    return required;
  }

  if (!buffer || buffer_size < required)
    return 0;  // insufficient buffer

  /* The call to ConvertStringToUnicode above may have messed with this value, it likely won't
     matter but reset it to its original value anyhow. */
  srcSize_uint = UINT(bytesRead);
  dstSize = UINT(buffer_size - 1); /* -1 so there room left for a zero terminator. */
  hr = pMLang->ConvertStringToUnicode(
      &mode, codePage, reinterpret_cast<CHAR *>(raw.data()), &srcSize_uint, buffer, &dstSize);
  if (FAILED(hr))
    return 0;

  buffer[dstSize] = L'\0';
  return size_t(dstSize) + 1;
}

#else

/* intentionally empty for UNIX */

#endif
