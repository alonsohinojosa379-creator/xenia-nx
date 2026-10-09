# Nintendo Switch front-ends.
#
# xenia-ppc-tests-nx: runs Xenia's PowerPC instruction test-suite through the
# ARM64 JIT on real Switch hardware. This is the Stage 1 bring-up target: if it
# passes, the CPU core (PPC -> A64 recompiler, guest memory, exceptions)
# works on Horizon.

add_executable(xenia-ppc-tests-nx
  ${CMAKE_CURRENT_SOURCE_DIR}/ppc_tests_nx_main.cc
  # xex_module.cc (part of xenia-cpu) instantiates the default XAM user
  # settings from kernel/xam/user_settings.h; the kernel is not built yet.
  ${PROJECT_SOURCE_DIR}/src/xenia/kernel/xam/user_data.cc
  ${PROJECT_SOURCE_DIR}/src/xenia/kernel/xam/user_settings.cc
)
target_link_libraries(xenia-ppc-tests-nx PRIVATE
  xenia-core xenia-cpu xenia-cpu-backend-a64 xenia-base
  capstone fmt mspack xbyak_aarch64
)
xe_target_defaults(xenia-ppc-tests-nx)

nx_generate_nacp(OUTPUT xenia-ppc-tests-nx.nacp
  NAME "Xenia NX - PPC tests"
  AUTHOR "Asce22"
  VERSION "0.1.0"
)
nx_create_nro(xenia-ppc-tests-nx
  NACP xenia-ppc-tests-nx.nacp
)
