# Copyright 2026 The Phantasos Authors
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

function(phs_check_standard_library)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fno-exceptions -fno-rtti")
    if(WIN32)
        set(CMAKE_REQUIRED_DEFINITIONS -D_HAS_EXCEPTIONS=0)
    endif()
    # A compile-only probe would miss missing runtime symbols (e.g. std::print).
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_stdlib.cpp")
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_stdlib.cpp" phs_stdlib_probe)
    # Recheck when a user changes compiler flags or standard-library selection.
    unset(PHS_HAS_CPP23_STDLIB CACHE)
    check_cxx_source_compiles("${phs_stdlib_probe}" PHS_HAS_CPP23_STDLIB)
    if(NOT PHS_HAS_CPP23_STDLIB)
        message(FATAL_ERROR
            "The selected compiler and C++ standard library cannot compile and link stage0's C++23 features "
            "(std::expected, std::print, std::format, ranges::to, views::join_with, floating-point from_chars). "
            "Clang's version alone does not determine library support. "
            "On Ubuntu use clang++-19 with g++-14/libstdc++-14 and "
            "-DCMAKE_CXX_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14; "
            "on Windows use a current MSVC STL with clang++. "
            "See CMakeFiles/CMakeConfigureLog.yaml for the failed compile/link diagnostics.")
    endif()
endfunction()

phs_check_standard_library()
