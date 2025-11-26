/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "testing/testing.h"

#include "BLI_debugger_utils.h"

#include <iostream>

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <tchar.h>
#  include <windows.h>
#  define pid_t DWORD
// Manipulate PEB structure to make windows think this process is being debugged.
// For more information, see replies to https://stackoverflow.com/q/76229564
#  include <winternl.h>
#  pragma comment(lib, "ntdll.lib")
extern "C" PPEB WINAPI RtlGetCurrentPeb();
#else
#  include <sys/ptrace.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

void start_tracing();
void stop_tracing(pid_t pid);

enum { PRESENT = 1, NOT_PRESENT };

void start_tracing()
{
#ifdef WIN32
  PPEB pPeb = RtlGetCurrentPeb();
  pPeb->BeingDebugged = TRUE;
#elif defined(__APPLE__)
  ptrace(PT_TRACE_ME, 0, NULL, 0);
#else
  ptrace(PTRACE_TRACEME, 0, NULL, NULL);
#endif
}

void stop_tracing(pid_t pid)
{
#ifdef WIN32
  PPEB pPeb = RtlGetCurrentPeb();
  pPeb->BeingDebugged = FALSE;
#elif defined(__APPLE__)
  ptrace(PT_DETACH, 0, NULL, 0);
#else
  ptrace(PTRACE_DETACH, pid, NULL, NULL);
#endif
}

namespace blender::tests {

TEST(debugger_utils, debugger_present)
{
  if (BLI_debugger_present()) {
    GTEST_SKIP() << "Running this test under a debugger will produce incorrect results, skipping";
  }

  EXPECT_FALSE(BLI_debugger_present());

#ifdef WIN32
  start_tracing();
  ASSERT_TRUE(IsDebuggerPresent());
  stop_tracing(0);
#else
  pid_t pid = fork();
  if (pid < 0) {
    GTEST_FAIL() << "fork() failed with code " << pid;
  }
  else if (pid == 0) {  // child
    start_tracing();
    exit(BLI_debugger_present() ? PRESENT : NOT_PRESENT);
  }
  // parent
  int status = 0;
  wait(&status);
  int child_rc = WEXITSTATUS(status);

  // process should have exited gracefully
  EXPECT_FALSE(WIFSTOPPED(status));
  EXPECT_EQ(child_rc, PRESENT);
#endif

  // make sure that we aren't being traced by parent process
  EXPECT_FALSE(BLI_debugger_present());
}

TEST(debugger_utils, debugger_breakpoint)
{
  if (BLI_debugger_present()) {
    GTEST_SKIP() << "Running this test under a debugger will produce incorrect results, skipping";
  }
  EXPECT_EQ(BLI_debugger_breakpoint(NULL), -1);

#ifdef WIN32
// skip breakpoint generation on windows since calling DebugBreak()
// results in abort() being called after start_tracing()
#else
  pid_t pid = fork();
  if (pid < 0) {
    GTEST_FAIL() << "fork() failed with code " << pid;
  }
  else if (pid == 0) {  // child
    start_tracing();
    EXPECT_TRUE(BLI_debugger_present());
    exit((BLI_debugger_breakpoint(NULL) == 0) ? PRESENT : NOT_PRESENT);
  }
  // parent
  int status = 0;
  wait(&status);

  // process should have been stopped by SIGTRAP
  EXPECT_TRUE(WIFSTOPPED(status));
#endif

  EXPECT_EQ(BLI_debugger_breakpoint(NULL), -1);
}
}  // namespace blender::tests
