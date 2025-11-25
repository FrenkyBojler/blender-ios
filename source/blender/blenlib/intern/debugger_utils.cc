/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__)
#  define __APPLE_API_UNSTABLE
#  include <csignal>
#  include <sys/sysctl.h>
#  include <unistd.h>
#elif defined(__linux__)
#  include <cctype>
#  include <charconv>
#  include <csignal>
#  include <fstream>
#  include <string_view>
#endif

#include "BLI_debugger_utils.h"

#include "CLG_log.h"

static CLG_LogRef LOG = {"lib.debugger"};

/** \file
 * \ingroup bli
 */

bool BLI_debugger_present()
{
#ifdef WIN32
  return IsDebuggerPresent();
#elif defined(__APPLE__)
  // similar to the AmIBeingDebugged function in an
  // Apple technical Q&A
  // https://developer.apple.com/library/archive/qa/qa1361/_index.html
  int mib[4] = {0};
  size_t len = 4;
  sysctlnametomib("kern.proc.pid", &mib, &len);
  if (mib != 3) {  // didn't initialize correctly
    goto error;
  }
  mib[3] = getpid();
  struct kinfo_proc kp = {0};
  kp.kp_proc.p_flag = 0;
  len = sizeof(kp);
  if (sysctl(mid, 4, &kp, &len, NULL, 0) != 0) {
    goto error;
  }
  return (kp.kp_proc.p_flag & P_TRACED) == P_TRACED;

error:
  static bool warning_shown = false;
  if (!warning_shown) {
    CLOG_STR_DEBUG(&LOG, "Could not determine if debugger is present");
    warning_shown = true;
  }
  return false;
#elif defined(__linux__)
  // check /proc/self/status to see if TracerPid is non-zero,
  // which has been in procfs since linux 2.6.  For more information
  // see https://stackoverflow.com/a/24969863
  std::ifstream proc_status("/proc/self/status");
  std::string line;
  while (!proc_status.eof()) {
    std::getline(proc_status, line);
    std::string_view sv(line.c_str(), 9);  // strlen("TracerPid") = 9
    if (sv != "TracerPid") {
      continue;
    }
    std::string::iterator pid_begin = line.begin();
    while (!std::isdigit(*pid_begin) && pid_begin != line.end()) {
      pid_begin++;
    }
    sv = std::string_view(&(*pid_begin));

    int tracer_pid = 0;
    (void)std::from_chars(sv.data(), sv.data() + sv.length(), tracer_pid);
    return (tracer_pid > 0) ? true : false;
  }
#endif
  // unknown OS
  return false;
}

int BLI_debugger_breakpoint(const char *name)
{
  if (!BLI_debugger_present()) {
    return -1;
  }
  if (name) {
    CLOG_DEBUG(&LOG, "Creating debugger breakpoint: (%s)", name);
  }
  else {
    CLOG_STR_DEBUG(&LOG, "Creating debugger breakpoint");
  }
#ifdef WIN32
  // Wrap DebugBreak in an exception handler so that something going wrong
  // doesn't crash the process
  __try
  {
    DebugBreak();
    return 0;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    CLOG_WARN(&LOG, "DebugBreak() failed with code %u", GetLastError());
    return -1;
  }
#else  // POSIX-based
  std::raise(SIGTRAP);
#endif
  return 0;
}
