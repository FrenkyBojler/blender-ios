/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup win32oskey
 */
#include <cinttypes>
#include <iostream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "BLI_subprocess.hh"

#include "win32oskey.hh"

#if BLI_SUBPROCESS_SUPPORT

static void print_last_error(const char *function, const char *msg)
{
  DWORD error_code = GetLastError();
  std::cerr << "ERROR (" << error_code << "): " << function << " : " << msg << std::endl;
}

static void check(bool result, const char *function, const char *msg)
{
  if (!result) {
    print_last_error(function, msg);
    BLI_assert(false);
  }
}

#  define CHECK(result) check((result), __func__, #result)
#  undef ERROR /* Defined in wingdi.h */
#  define ERROR(msg) check(false, __func__, msg)

static DWORD parent_pid{};
static HANDLE enable_suppression_event{};
static blender::BlenderSubprocess subprocess{};

const char *const blender::win32oskey::arg_name = "--oskey-start-menu-suppression-subprocess";

void blender::win32oskey::enable_suppression(bool enable)
{
  /* If subprocess isn't running and the feature is not enabled, do nothing. */
  if (!enable && !subprocess.is_running()) {
    return;
  }

  if (!enable_suppression_event) {
    enable_suppression_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!enable_suppression_event) {
      ERROR("CreateEventW");
      return;
    }
  }

  if (enable) {
    if (!SetEvent(enable_suppression_event)) {
      ERROR("SetEvent");
    }
  }
  else {
    if (!ResetEvent(enable_suppression_event)) {
      ERROR("ResetEvent");
    }
  }

  if (!subprocess.is_running()) {
    /* Reset the object. */
    subprocess.~BlenderSubprocess();
    new (&subprocess) BlenderSubprocess();

    HANDLE dup_current_process;
    if (!DuplicateHandle(GetCurrentProcess(),
                         GetCurrentProcess(),
                         GetCurrentProcess(),
                         &dup_current_process,
                         0,
                         TRUE,
                         DUPLICATE_SAME_ACCESS))
    {
      ERROR("DuplicateHandle(current process)");
      return;
    }

    HANDLE dup_enable_suppression_event;
    if (!DuplicateHandle(GetCurrentProcess(),
                         enable_suppression_event,
                         GetCurrentProcess(),
                         &dup_enable_suppression_event,
                         0,
                         TRUE,
                         DUPLICATE_SAME_ACCESS))
    {
      ERROR("DuplicateHandle(enable suppression event)");
      CHECK(CloseHandle(dup_current_process));
      return;
    }

    subprocess.create(
        {
            arg_name,
            std::to_string(reinterpret_cast<uintptr_t>(dup_current_process)),
            std::to_string(reinterpret_cast<uintptr_t>(dup_enable_suppression_event)),
        },
        {dup_current_process, dup_enable_suppression_event});

    CHECK(CloseHandle(dup_current_process));
    CHECK(CloseHandle(dup_enable_suppression_event));
  }
}

static LRESULT CALLBACK low_level_keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam)
{
  constexpr WORD START_MENU_SUPPRESSION_EXTRA_KEY = VK_NONAME;

  PKBDLLHOOKSTRUCT p = reinterpret_cast<PKBDLLHOOKSTRUCT>(lParam);

  if (nCode == HC_ACTION && wParam == WM_KEYUP && (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) &&
      !GetAsyncKeyState(START_MENU_SUPPRESSION_EXTRA_KEY) &&
      WaitForSingleObject(enable_suppression_event, 0) == WAIT_OBJECT_0)
  {
    DWORD foreground_pid;
    if (GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid) &&
        foreground_pid == parent_pid)
    {
      INPUT inputs[3]{};
      inputs[0].ki.time = inputs[1].ki.time = inputs[2].ki.time = p->time;
      inputs[0].type = inputs[1].type = inputs[2].type = INPUT_KEYBOARD;
      inputs[0].ki.wVk = START_MENU_SUPPRESSION_EXTRA_KEY;
      inputs[1].ki.wVk = static_cast<WORD>(p->vkCode);
      inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
      inputs[2].ki.wVk = START_MENU_SUPPRESSION_EXTRA_KEY;
      inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(std::size(inputs), inputs, sizeof(inputs[0]));
      return 1;
    }
  }

  return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void blender::win32oskey::subprocess_run(const char *parent_handle_string,
                                         const char *enable_suppression_handle_string)
{
  HANDLE parent = reinterpret_cast<HANDLE>(strtoumax(parent_handle_string, nullptr, 0));
  enable_suppression_event = reinterpret_cast<HANDLE>(
      strtoumax(enable_suppression_handle_string, nullptr, 0));

  /* Parent handle is expected to be a process.
   * If something else is given, do nothing. */
  parent_pid = GetProcessId(parent);
  if (parent_pid == 0) {
    ERROR("GetProcessId");
    return;
  }

  HHOOK hhk = SetWindowsHookExW(WH_KEYBOARD_LL, &low_level_keyboard_hook_proc, nullptr, 0);
  if (!hhk) {
    ERROR("SetWindowsHookEx(WH_KEYBOARD_LL)");
    return;
  }

  for (MSG msg{}; GetMessageW(&msg, nullptr, 0, 0);) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  UnhookWindowsHookEx(hhk);
}

#endif
