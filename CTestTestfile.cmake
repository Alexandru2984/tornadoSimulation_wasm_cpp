# CMake generated Testfile for 
# Source directory: /home/runner/work/tornadoSimulation_wasm_cpp/tornadoSimulation_wasm_cpp
# Build directory: /home/runner/work/tornadoSimulation_wasm_cpp/tornadoSimulation_wasm_cpp/build-wasm
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(test_math "/home/runner/work/_temp/f2d13cc8-6185-4953-b40a-9b2434a81492/emsdk-main/node/22.16.0_64bit/bin/node" "/home/runner/work/tornadoSimulation_wasm_cpp/tornadoSimulation_wasm_cpp/build-wasm/test_math.js")
set_tests_properties(test_math PROPERTIES  _BACKTRACE_TRIPLES "/home/runner/work/tornadoSimulation_wasm_cpp/tornadoSimulation_wasm_cpp/CMakeLists.txt;130;add_test;/home/runner/work/tornadoSimulation_wasm_cpp/tornadoSimulation_wasm_cpp/CMakeLists.txt;0;")
subdirs("_deps/glm-build")
