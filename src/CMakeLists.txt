set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

add_executable(clang-tags main.cpp)

execute_process(
  COMMAND llvm-config --cxxflags OUTPUT_VARIABLE LLVM_CXX_FLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
  COMMAND llvm-config --ldflags --libs OUTPUT_VARIABLE LLVM_LINK_FLAGS
  OUTPUT_STRIP_TRAILING_WHITESPACE)

string(REPLACE "-O3" "" LLVM_CXX_FLAGS ${LLVM_CXX_FLAGS})
string(REPLACE "-NDEBUG" "" LLVM_CXX_FLAGS ${LLVM_CXX_FLAGS})
string(REPLACE "\n" " " LLVM_LINK_FLAGS ${LLVM_LINK_FLAGS})

set_source_files_properties(main.cpp PROPERTIES COMPILE_FLAGS ${LLVM_CXX_FLAGS})

add_definitions("-fno-rtti")

set_target_properties(clang-tags PROPERTIES COMPILE_FLAGS ${LLVM_CXX_FLAGS})
set_target_properties(clang-tags PROPERTIES LINK_FLAGS ${LLVM_LINK_FLAGS})

target_link_libraries(clang-tags
  clangARCMigrate
  clangAST
  clangAnalysis
  clangBasic
  clangCodeGen
  clangDriver
  clangEdit
  clangFrontend
  clangFrontendTool
  clangLex
  clangParse
  clangRewrite
  clangSema
  clangSerialization
  clangStaticAnalyzerCheckers
  clangStaticAnalyzerCore
  clangStaticAnalyzerFrontend
  clangTooling

  sqlite3)
