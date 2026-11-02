# RawSpeed (third_party/rawspeed, darktable's decoder, a submodule) and
# pugixml (third_party/pugixml), which reads RawSpeed's camera list, compiled
# into the port from their sources. RawSpeed's own CMake is written for GCC's
# and Clang's command line, not clang-cl's, and builds its tests, tools and
# fuzzers too. The settings are those of its portable binary-package build
# (BINARY_PACKAGE_BUILD), without OpenMP, zlib or libjpeg for now: Deflate and
# lossy-JPEG DNGs need the last two.
set(RAWSPEED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rawspeed)
file(GLOB_RECURSE RAWSPEED_SOURCES CONFIGURE_DEPENDS ${RAWSPEED_DIR}/src/librawspeed/*.cpp)

set(RAWSPEED_CACHELINESIZE 64)
set(RAWSPEED_PAGESIZE 4096)
set(RAWSPEED_LARGEPAGESIZE 4096)
set(WITH_SSE2 ON)                   # the header uses it only where __SSE2__ is defined
set(HAVE_PUGIXML ON)
set(HAVE_CXX_THREAD_LOCAL ON)
configure_file(${RAWSPEED_DIR}/src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/rawspeed/rawspeedconfig.h)

add_library(flx_rawspeed STATIC ${RAWSPEED_SOURCES} third_party/pugixml/src/pugixml.cpp)
target_include_directories(flx_rawspeed PUBLIC
    ${RAWSPEED_DIR}/src/librawspeed
    ${RAWSPEED_DIR}/src/external
    ${CMAKE_CURRENT_BINARY_DIR}/rawspeed
    third_party/pugixml/src)
target_compile_definitions(flx_rawspeed PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(flx_rawspeed PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(flx_rawspeed PRIVATE /w)
else()
    target_compile_options(flx_rawspeed PRIVATE -w)
endif()

# the camera list, next to the programs that read it
configure_file(${RAWSPEED_DIR}/data/cameras.xml ${CMAKE_CURRENT_BINARY_DIR}/cameras.xml COPYONLY)
