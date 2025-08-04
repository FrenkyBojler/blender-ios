# SPDX-FileCopyrightText: 2025 Blender Foundation
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  PUBLIC include
)

set(INC_SYS

)

set(SRC
  src/aabb.c
  src/aabb.h
  src/arena_allocator.c
  src/arena_allocator.h
  src/array.c
  src/array.h
  src/atomic.h
  src/bitset.c
  src/bitset.h
  src/body.c
  src/body.h
  src/broad_phase.c
  src/broad_phase.h
  src/constants.h
  src/constraint_graph.c
  src/constraint_graph.h
  src/contact.c
  src/contact.h
  src/contact_solver.c
  src/contact_solver.h
  src/core.c
  src/core.h
  src/ctz.h
  src/distance.c
  src/distance_joint.c
  src/dynamic_tree.c
  src/geometry.c
  src/hull.c
  src/id_pool.c
  src/id_pool.h
  src/island.c
  src/island.h
  src/joint.c
  src/joint.h
  src/manifold.c
  src/math_functions.c
  src/motor_joint.c
  src/mouse_joint.c
  src/mover.c
  src/physics_world.c
  src/physics_world.h
  src/prismatic_joint.c
  src/revolute_joint.c
  src/sensor.c
  src/sensor.h
  src/shape.c
  src/shape.h
  src/solver.c
  src/solver.h
  src/solver_set.c
  src/solver_set.h
  src/table.c
  src/table.h
  src/timer.c
  src/types.c
  src/weld_joint.c
  src/wheel_joint.c

  include/box2d/base.h
  include/box2d/box2d.h
  include/box2d/collision.h
  include/box2d/id.h
  include/box2d/math_functions.h
  include/box2d/types.h
)

set(LIB
)

set(CMAKE_C_STANDARD 17)

blender_add_lib(extern_box2d "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
add_library(bf::extern::box2d ALIAS extern_box2d)
