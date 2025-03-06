#
#
#


set(INC
  .
  ../blenlib
)

set(LIB
  PRIVATE bf::blenlib
)


list(APPEND INC_SYS
    # Not sure if needed
   ${BOOST_INCLUDE_DIR}
)

# Auto fill source files, so patches will not touch that file
file(GLOB sources "MB_*")
foreach(source ${sources})
  get_filename_component(name ${source} NAME)
  list(APPEND SRC "${name}")
endforeach()

# Generate a Definition for each found patch
file(GLOB patches "patches/*")
 foreach(patch ${patches})
  get_filename_component(def ${patch} NAME_WE)
  get_filename_component(name ${patch} NAME)
  if (${name} MATCHES "^MB_[0-9][0-9][0-9][0-9]\.h$" )
    add_definitions(-D${def})
    list(APPEND SRC "patches/${name}")
  else()
    message (FATAL_ERROR  "invalid patch file ${name}")
  endif ()
endforeach()


blender_add_lib(tornavis "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
