add_library(tracing 
  tracing.cc
  tracing.hh
  internal/tracing_internal.cc
  internal/tracing_internal.hh
)

target_include_directories(tracing 
  PUBLIC .
  PRIVATE internal
)

if(WITH_LAZY_TRACE)
  target_compile_definitions(tracing PUBLIC WITH_LAZY_TRACE)
endif()

add_library(bf::intern::lazytrace ALIAS tracing)