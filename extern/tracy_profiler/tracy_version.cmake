# Separate file used for defining the Tracy version as it is used by both:
#   - The Tracy Client library, built as a pre-compiled library in build_files/build_environment/cmake/tracy.cmake
#   - The Tracy Profiler server standalone client GUI, built as an standalone target in extern/tracy_profiler

# Using a latest main hash as Tracy WoA support (commit feb07e4) hasn't made it to a stable release yet. Switch to stable on next upgrade.
set(TRACY_VERSION 00a069d6088ff8d93304eaac4d925cece0e9081c) # Latest main on 2026-04-09
