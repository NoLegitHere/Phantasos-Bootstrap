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

cmake_minimum_required(VERSION 3.28)
project(phantasos_stage0 VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "stage0 must be built with clang++ (found ${CMAKE_CXX_COMPILER_ID})")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
    message(FATAL_ERROR "stage0 requires clang++ >= 18 (found ${CMAKE_CXX_COMPILER_VERSION})")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Stage0 uses headers, so it does not need the separate clang-scan-deps tool.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/CheckStandardLibrary.cmake)

option(PHS_WERROR "Treat warnings as errors" OFF)

# Warnings belong to our targets; language/ABI requirements follow the library
# into consumers, including the CLI and the focused tests.
function(phs_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wno-missing-field-initializers
        $<$<BOOL:${PHS_WERROR}>:-Werror>
    )
endfunction()

add_library(phantasos_frontend STATIC
    src/frontend/session.cpp
    src/support/source.cpp
    src/support/diag.cpp
    src/syntax/token.cpp
    src/syntax/lexer.cpp
    src/syntax/parser.cpp
    src/syntax/ast_dump.cpp
    src/sema/validate.cpp
    src/sema/resolve.cpp
    src/sema/types.cpp
    src/sema/aliases.cpp
    src/sema/type_table.cpp
    src/sema/flow.cpp
    src/sema/exhaustive.cpp
    src/sema/size.cpp
    src/codegen/gate.cpp
    src/codegen/lower.cpp
    src/codegen/mir.cpp
    src/codegen/elf.cpp
    src/codegen/x86_64.cpp
)
add_library(Phantasos::Frontend ALIAS phantasos_frontend)
target_include_directories(phantasos_frontend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(phantasos_frontend PUBLIC cxx_std_23)
target_compile_definitions(phantasos_frontend PUBLIC
    # Tell the MSVC STL (used by clang on Windows) that exceptions are disabled.
    $<$<PLATFORM_ID:Windows>:_HAS_EXCEPTIONS=0>
)
target_compile_options(phantasos_frontend PUBLIC -fno-exceptions -fno-rtti)
phs_warnings(phantasos_frontend)

add_executable(phantasos-stage0 src/main.cpp)
target_link_libraries(phantasos-stage0 PRIVATE Phantasos::Frontend)
target_compile_definitions(phantasos-stage0 PRIVATE PHS_VERSION="${PROJECT_VERSION}")
phs_warnings(phantasos-stage0)

include(CTest)
if(BUILD_TESTING)
    foreach(suite IN ITEMS support frontend sema resolve session types calls conversions aliases composites tuples members hierarchy generics inference bodies closures flow gate mir x86_64 elf)
        add_executable(phantasos-${suite}-tests tests/unit/${suite}_tests.cpp)
        target_link_libraries(phantasos-${suite}-tests PRIVATE Phantasos::Frontend)
        phs_warnings(phantasos-${suite}-tests)
        add_test(NAME ${suite} COMMAND phantasos-${suite}-tests)
    endforeach()
    # Bound descriptor, member and inheritance graph regressions.
    set_tests_properties(composites members hierarchy generics inference bodies closures flow gate mir elf PROPERTIES TIMEOUT 30)
    # The x86_64 suite runs every program twice, natively and in the
    # interpreter, and takes over 20 s under sanitizers.
    set_tests_properties(x86_64 PROPERTIES TIMEOUT 60)
endif()
