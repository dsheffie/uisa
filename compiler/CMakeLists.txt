cmake_minimum_required(VERSION 3.16)
project(ucc CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(FLEX REQUIRED)
find_package(BISON 3.0 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# z3 (MacPorts installs it under /opt/local; Homebrew under /opt/homebrew)
find_path(Z3_INCLUDE_DIR z3++.h HINTS /opt/local/include /opt/homebrew/include /usr/local/include)
find_library(Z3_LIBRARY z3 HINTS /opt/local/lib /opt/homebrew/lib /usr/local/lib)
if(NOT Z3_INCLUDE_DIR OR NOT Z3_LIBRARY)
  message(FATAL_ERROR "ucc needs z3 (z3++.h and libz3), e.g. `sudo port install z3`")
endif()

set(SPEC ${CMAKE_CURRENT_SOURCE_DIR}/../spec/uisa.yaml)
set(INTRINSICS ${CMAKE_CURRENT_BINARY_DIR}/uisa_intrinsics.inc)
add_custom_command(
  OUTPUT ${INTRINSICS}
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gen_intrinsics.py ${INTRINSICS}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_intrinsics.py ${SPEC} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/uisa.py
  COMMENT "Generating the intrinsic table from spec/uisa.yaml")

BISON_TARGET(parser parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.hpp)
FLEX_TARGET(scanner scanner.l ${CMAKE_CURRENT_BINARY_DIR}/scanner.cpp)
ADD_FLEX_BISON_DEPENDENCY(scanner parser)

add_executable(ucc
  main.cpp ast.cpp tac.cpp codegen.cpp backend.cpp regalloc.cpp
  ${BISON_parser_OUTPUTS} ${FLEX_scanner_OUTPUTS} ${INTRINSICS})
target_include_directories(ucc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR} ${Z3_INCLUDE_DIR})
target_link_libraries(ucc PRIVATE ${Z3_LIBRARY})
target_compile_options(ucc PRIVATE -Wall -Wno-unused-function -Wno-sign-compare)
set_source_files_properties(${FLEX_scanner_OUTPUTS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-deprecated-register")
