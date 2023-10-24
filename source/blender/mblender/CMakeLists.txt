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


if(WITH_BOOST)
  list(APPEND INC_SYS
    ${BOOST_INCLUDE_DIR}
  )
else()
	message (FATAL_ERROR "mblender requires WITH_BOOST")
endif()

# Auto fill source files, so patches will not touch that file
file(GLOB sources "MB_*")
foreach(source ${sources})
  get_filename_component(name ${source} NAME)
  list(APPEND SRC "${name}")
endforeach()

# Generate a Definition for each found patch
file(GLOB patches "patches/*.h")
 foreach(patch ${patches})
  get_filename_component(def ${patch} NAME_WE)
  get_filename_component(name ${patch} NAME)
  add_definitions(-D${def})
  # list(APPEND SRC "patches/${name}")
endforeach()


blender_add_lib(mblender "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")
