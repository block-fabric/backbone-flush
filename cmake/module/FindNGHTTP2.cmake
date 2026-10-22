# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

#[=======================================================================[
FindNGHTTP2
-----------

Finds libnghttp2 (the HTTP/2 library of the gRPC server, WITH_GRPC), with pkg-config if it can,
else by its header and library (depends removes the .pc files), and makes the target nghttp2.

#]=======================================================================]

include(FindPackageHandleStandardArgs)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_NGHTTP2 QUIET libnghttp2)
endif()
find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h HINTS ${PC_NGHTTP2_INCLUDE_DIRS})
find_library(NGHTTP2_LIBRARY NAMES nghttp2 HINTS ${PC_NGHTTP2_LIBRARY_DIRS})
if(NGHTTP2_INCLUDE_DIR AND EXISTS "${NGHTTP2_INCLUDE_DIR}/nghttp2/nghttp2ver.h")
  file(STRINGS "${NGHTTP2_INCLUDE_DIR}/nghttp2/nghttp2ver.h" _ver REGEX "^#define NGHTTP2_VERSION \"[^\"]*\"")
  string(REGEX REPLACE ".*\"([^\"]*)\".*" "\\1" NGHTTP2_VERSION "${_ver}")
endif()
find_package_handle_standard_args(NGHTTP2
  REQUIRED_VARS NGHTTP2_LIBRARY NGHTTP2_INCLUDE_DIR
  VERSION_VAR NGHTTP2_VERSION
)
if(NGHTTP2_FOUND AND NOT TARGET nghttp2)
  add_library(nghttp2 UNKNOWN IMPORTED)
  set_target_properties(nghttp2 PROPERTIES
    IMPORTED_LOCATION "${NGHTTP2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NGHTTP2_INCLUDE_DIR}"
  )
  # A static libnghttp2 (depends) needs NGHTTP2_STATICLIB on Windows, for its declarations.
  if(NGHTTP2_LIBRARY MATCHES "\\.a$")
    set_target_properties(nghttp2 PROPERTIES INTERFACE_COMPILE_DEFINITIONS NGHTTP2_STATICLIB)
  endif()
endif()
mark_as_advanced(NGHTTP2_INCLUDE_DIR NGHTTP2_LIBRARY)
