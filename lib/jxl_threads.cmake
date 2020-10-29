# Copyright (c) the JPEG XL Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

find_package(Threads REQUIRED)

### Define the jxl_threads shared or static target library. The ${target}
# parameter should already be created with add_library(), but this function
# sets all the remaining common properties.
function(_set_jxl_threads _target)
target_sources(${_target} PRIVATE
  threads/thread_parallel_runner.cc
  threads/thread_parallel_runner_internal.cc
)

target_compile_options(${_target} PRIVATE ${JPEGXL_INTERNAL_FLAGS})
target_compile_options(${_target} PUBLIC ${JPEGXL_COVERAGE_FLAGS})
set_property(TARGET ${_target} PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(${_target}
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/include")

target_link_libraries(${_target}
  PUBLIC ${JPEGXL_COVERAGE_FLAGS} Threads::Threads
)

set_target_properties(${_target} PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN 1
  DEFINE_SYMBOL JXL_THREADS_INTERNAL_LIBRARY_BUILD
)

# Always install the library as jxl_threads.{a,so} file without the "-static"
# suffix, except in Windows.
if (NOT WIN32)
  set_target_properties(${_target} PROPERTIES OUTPUT_NAME "jxl_threads")
endif()
install(TARGETS ${_target} DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(MINGW)
target_link_libraries(${_target} PUBLIC mingw_stdthreads)
endif()

endfunction()


### Static library.
add_library(jxl_threads-static STATIC)
_set_jxl_threads(jxl_threads-static)

# Make jxl_threads symbols neither imported nor exported when using the static
# library. These will have hidden visibility anyway in the static library case
# in unix.
target_compile_definitions(jxl_threads-static
  PUBLIC -DJXL_THREADS_STATIC_DEFINE)


### Public shared library.
if ((NOT DEFINED "${TARGET_SUPPORTS_SHARED_LIBS}") OR "${TARGET_SUPPORTS_SHARED_LIBS}")
add_library(jxl_threads SHARED)
_set_jxl_threads(jxl_threads)

set_target_properties(jxl_threads PROPERTIES
  VERSION ${JPEGXL_LIBRARY_VERSION}
  SOVERSION ${JPEGXL_LIBRARY_SOVERSION}
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Compile the shared library such that the JXL_THREADS_EXPORT symbols are
# exported. Users of the library will not set this flag and therefore import
# those symbols.
target_compile_definitions(jxl_threads
  PRIVATE -DJXL_THREADS_INTERNAL_LIBRARY_BUILD)

# Generate the jxl/jxl_threads_export.h header, we only need to generate it once
# but we can use it from both libraries.
generate_export_header(jxl_threads
  BASE_NAME JXL_THREADS
  EXPORT_FILE_NAME include/jxl/jxl_threads_export.h)
else()
# When not building the shared library generate the jxl_threads_export.h header
# only based on the static target.
generate_export_header(jxl_threads-static
  BASE_NAME JXL_THREADS
  EXPORT_FILE_NAME include/jxl/jxl_threads_export.h)
endif()  # TARGET_SUPPORTS_SHARED_LIBS


### Add a pkg-config file for libjxl_threads.
set(JPEGXL_THREADS_LIBRARY_REQUIRES "")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/threads/libjxl_threads.pc.in"
               "libjxl_threads.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libjxl_threads.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")