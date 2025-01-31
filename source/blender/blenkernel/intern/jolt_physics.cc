/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/* Jolt Physics Library (https://github.com/jrouwe/JoltPhysics)
 * SPDX-FileCopyrightText: 2021 Jorrit Rouwe
 * SPDX-License-Identifier: MIT */

/** \file
 * \ingroup bke
 */

#include "BKE_jolt_physics.hh"

#ifdef WITH_JOLT
/* Keep this before other Jolt includes. */
#  include <Jolt/Jolt.h>

#  include <Jolt/Core/Factory.h>
#  include <Jolt/RegisterTypes.h>
#endif

#include <cstdarg>
#include <iostream>

// Disable common warnings triggered by Jolt, you can use JPH_SUPPRESS_WARNING_PUSH /
// JPH_SUPPRESS_WARNING_POP to store and restore the warning state
JPH_SUPPRESS_WARNINGS

namespace blender::bke {

#ifdef WITH_JOLT
// Callback for traces, connect this to your own trace function if you have one
static void TraceImpl(const char *inFMT, ...)
{
  // Format the message
  va_list list;
  va_start(list, inFMT);
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), inFMT, list);
  va_end(list);

  // Print to the TTY
  std::cout << buffer << std::endl;
}

#  ifdef JPH_ENABLE_ASSERTS

// Callback for asserts, connect this to your own assert handler if you have one
static bool AssertFailedImpl(const char *inExpression,
                             const char *inMessage,
                             const char *inFile,
                             uint32_t inLine)
{
  //// Print to the TTY
  // cout << inFile << ":" << inLine << ": (" << inExpression << ") "
  //      << (inMessage != nullptr ? inMessage : "") << endl;

  // Breakpoint
  return true;
};

#  endif  // JPH_ENABLE_ASSERTS

void jolt_physics_init()
{
  // Register allocation hook. In this example we'll just let Jolt use malloc / free but you can
  // override these if you want (see Memory.h). This needs to be done before any other Jolt
  // function is called.
  JPH::RegisterDefaultAllocator();

  // Install trace and assert callbacks
  JPH::Trace = TraceImpl;
  JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)

  // Create a factory, this class is responsible for creating instances of classes based on their
  // name or hash and is mainly used for deserialization of saved data. It is not directly used in
  // this example but still required.
  JPH::Factory::sInstance = new JPH::Factory();

  // Register all physics types with the factory and install their collision handlers with the
  // CollisionDispatch class. If you have your own custom shape types you probably need to register
  // their handlers with the CollisionDispatch before calling this function. If you implement your
  // own default material (PhysicsMaterial::sDefault) make sure to initialize it before this
  // function or else this function will create one for you.
  JPH::RegisterTypes();
}

void jolt_physics_exit()
{
  // Unregisters all types with the factory and cleans up the default material
  JPH::UnregisterTypes();

  // Destroy the factory
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
}

#else

void jolt_physics_init() {}
void jolt_physics_exit() {}

#endif

}  // namespace blender::bke
