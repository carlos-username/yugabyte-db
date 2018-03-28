#########
# Local rewrite of the protobuf support in cmake.
#
# Supports cross-module protobuf dependencies and protobufs inside
# packages much better than the one built into cmake.
#########
#
# Locate and configure the Google Protocol Buffers library.
# Defines the following variables:
#
#   PROTOBUF_INCLUDE_DIR - the include directory for protocol buffers
#   PROTOBUF_SHARED_LIBRARY - path to protobuf's shared library
#   PROTOBUF_STATIC_LIBRARY - path to protobuf's static library
#   PROTOBUF_PROTOC_SHARED_LIBRARY - path to protoc's shared library
#   PROTOBUF_PROTOC_STATIC_LIBRARY - path to protoc's static library
#   PROTOBUF_PROTOC_EXECUTABLE - the protoc compiler
#   PROTOBUF_FOUND - whether the Protocol Buffers library has been found
#
#  ====================================================================
#  Example:
#
#   find_package(Protobuf REQUIRED)
#   include_directories(${PROTOBUF_INCLUDE_DIR})
#
#   include_directories(${CMAKE_CURRENT_BINARY_DIR})
#   PROTOBUF_GENERATE_CPP(PROTO_SRCS PROTO_HDRS PROTO_TGTS
#     [SOURCE_ROOT <root from which source is found>]
#     [BINARY_ROOT <root into which binaries are built>]
#     PROTO_FILES foo.proto)
#   add_executable(bar bar.cc ${PROTO_SRCS} ${PROTO_HDRS})
#   target_link_libraries(bar ${PROTOBUF_SHARED_LIBRARY})
#
# NOTE: You may need to link against pthreads, depending
# on the platform.
#  ====================================================================
#
# PROTOBUF_GENERATE_CPP (public function)
#   SRCS = Variable to define with autogenerated
#          source files
#   HDRS = Variable to define with autogenerated
#          header files
#   TGTS = Variable to define with autogenerated
#          custom targets; if SRCS/HDRS need to be used in multiple
#          libraries, those libraries should depend on these targets
#          in order to "serialize" the protoc invocations
#  ====================================================================

#=============================================================================
# Copyright 2011 Kirill A. Korinskiy <catap@catap.ru>
# Copyright 2009 Kitware, Inc.
# Copyright 2009 Philip Lowman <philip@yhbt.com>
# Copyright 2008 Esben Mose Hansen, Ange Optimization ApS
#
# Distributed under the OSI-approved BSD License (the "License");
# see accompanying file Copyright.txt for details.
#
# This software is distributed WITHOUT ANY WARRANTY; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
# See the License for more information.
#=============================================================================
# (To distributed this file outside of CMake, substitute the full
#  License text for the above reference.)

#
# The following only applies to changes made to this file as part of YugaByte development.
#
# Portions Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.  You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied.  See the License for the specific language governing permissions and limitations
# under the License.
#

# Convert a relative path to a .proto file to the name of a custom target used for generating the
# protobuf. This custom target enforces that there's just one invocation of protoc when there are
# multiple consumers of the generated files. The target name must be unique; adding parts of the
# filename helps ensure this.
#
# Example:
# PROTO_REL_TO_YB_SRC_ROOT=src/yb/util/jsonwriter_test.proto ->
#   TGT_NAME=gen_src_yb_util_jsonwriter_test_proto
# PROTO_REL_TO_YB_SRC_ROOT=src/yb/util/proto_container_test.proto ->
#   TGT_NAME=gen_src_yb_util_proto_container_test_proto
function(GET_PROTOBUF_GENERATION_TARGET_NAME PROTO_REL_TO_YB_SRC_ROOT OUTPUT_VAR_NAME)
  set(TGT_NAME "gen_${PROTO_REL_TO_YB_SRC_ROOT}")
  string(REPLACE "@" "_" TGT_NAME ${TGT_NAME})
  string(REPLACE "." "_" TGT_NAME ${TGT_NAME})
  string(REPLACE "-" "_" TGT_NAME ${TGT_NAME})
  string(REPLACE "/" "_" TGT_NAME ${TGT_NAME})
  string(TOLOWER "${TGT_NAME}" TGT_NAME)
  if (${YB_CMAKE_VERBOSE})
    message(
      "GET_PROTOBUF_GENERATION_TARGET_NAME: "
      "PROTO_REL_TO_YB_SRC_ROOT=${PROTO_REL_TO_YB_SRC_ROOT} -> "
      "TGT_NAME=${TGT_NAME}")
  endif()
  set("${OUTPUT_VAR_NAME}" "${TGT_NAME}" PARENT_SCOPE)
endfunction()

function(PROTOBUF_GENERATE_CPP SRCS HDRS TGTS)
  if(NOT ARGN)
    message(SEND_ERROR "Error: PROTOBUF_GENERATE_CPP() called without any proto files")
    return()
  endif(NOT ARGN)

  set(options)
  set(one_value_args SOURCE_ROOT BINARY_ROOT)
  set(multi_value_args EXTRA_PROTO_PATHS PROTO_FILES)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(SEND_ERROR "Error: unrecognized arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  set(${SRCS})
  set(${HDRS})
  set(${TGTS})

  set(EXTRA_PROTO_PATH_ARGS)
  foreach(PP ${ARG_EXTRA_PROTO_PATHS})
    set(EXTRA_PROTO_PATH_ARGS ${EXTRA_PROTO_PATH_ARGS} --proto_path ${PP})
  endforeach()

  if("${ARG_SOURCE_ROOT}" STREQUAL "")
    SET(ARG_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  GET_FILENAME_COMPONENT(ARG_SOURCE_ROOT ${ARG_SOURCE_ROOT} ABSOLUTE)

  if("${ARG_BINARY_ROOT}" STREQUAL "")
    SET(ARG_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  GET_FILENAME_COMPONENT(ARG_BINARY_ROOT ${ARG_BINARY_ROOT} ABSOLUTE)

  foreach(FIL ${ARG_PROTO_FILES})
    get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
    get_filename_component(FIL_WE ${FIL} NAME_WE)

    # Ensure that the protobuf file is within the source root.
    # This is a requirement of protoc.
    FILE(RELATIVE_PATH PROTO_REL_TO_ROOT "${ARG_SOURCE_ROOT}" "${ABS_FIL}")
    FILE(RELATIVE_PATH PROTO_REL_TO_YB_SRC_ROOT "${YB_SRC_ROOT}" "${ABS_FIL}")

    GET_FILENAME_COMPONENT(REL_DIR "${PROTO_REL_TO_ROOT}" PATH)

    if(NOT REL_DIR STREQUAL "")
      SET(REL_DIR "${REL_DIR}/")
    endif()

    set(PROTO_CC_OUT "${ARG_BINARY_ROOT}/${REL_DIR}${FIL_WE}.pb.cc")
    set(PROTO_H_OUT "${ARG_BINARY_ROOT}/${REL_DIR}${FIL_WE}.pb.h")
    list(APPEND ${SRCS} "${PROTO_CC_OUT}")
    list(APPEND ${HDRS} "${PROTO_H_OUT}")

    add_custom_command(
      OUTPUT "${PROTO_CC_OUT}" "${PROTO_H_OUT}"
      COMMAND  ${PROTOBUF_PROTOC_EXECUTABLE}
      ARGS
        --plugin $<TARGET_FILE:protoc-gen-insertions>
        --cpp_out ${ARG_BINARY_ROOT}
        --insertions_out ${ARG_BINARY_ROOT}
        --proto_path ${ARG_SOURCE_ROOT}
        # Used to find built-in .proto files (e.g. FileDescriptorProto)
        --proto_path ${PROTOBUF_INCLUDE_DIR}
        ${EXTRA_PROTO_PATH_ARGS} ${ABS_FIL}
      DEPENDS ${ABS_FIL} protoc-gen-insertions
      COMMENT "Running C++ protocol buffer compiler on ${FIL}"
      VERBATIM )

    GET_PROTOBUF_GENERATION_TARGET_NAME("${PROTO_REL_TO_YB_SRC_ROOT}" TGT_NAME)
    add_custom_target(${TGT_NAME}
      DEPENDS "${PROTO_CC_OUT}" "${PROTO_H_OUT}" protoc-gen-insertions)
    add_dependencies(gen_proto ${TGT_NAME})
    list(APPEND ${TGTS} "${TGT_NAME}")
  endforeach()

  set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE)
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
  set(${TGTS} ${${TGTS}} PARENT_SCOPE)
endfunction()


find_path(PROTOBUF_INCLUDE_DIR google/protobuf/service.h
  NO_CMAKE_SYSTEM_PATH
  NO_SYSTEM_ENVIRONMENT_PATH)

find_library(PROTOBUF_SHARED_LIBRARY protobuf
             DOC "The Google Protocol Buffers Library"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

find_library(PROTOBUF_STATIC_LIBRARY libprotobuf.a
             DOC "Static version of the Google Protocol Buffers Library"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

find_library(PROTOBUF_PROTOC_SHARED_LIBRARY protoc
             DOC "The Google Protocol Buffers Compiler Library"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

find_library(PROTOBUF_PROTOC_STATIC_LIBRARY libprotoc.a
             DOC "Static version of the Google Protocol Buffers Compiler Library"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

find_program(PROTOBUF_PROTOC_EXECUTABLE protoc
             DOC "The Google Protocol Buffers Compiler"
             NO_CMAKE_SYSTEM_PATH
             NO_SYSTEM_ENVIRONMENT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(PROTOBUF REQUIRED_VARS
  PROTOBUF_SHARED_LIBRARY PROTOBUF_STATIC_LIBRARY
  PROTOBUF_PROTOC_SHARED_LIBRARY PROTOBUF_PROTOC_STATIC_LIBRARY
  PROTOBUF_INCLUDE_DIR PROTOBUF_PROTOC_EXECUTABLE)
