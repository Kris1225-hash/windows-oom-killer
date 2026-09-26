cmake_minimum_required(VERSION 3.16)
project(windows_oom_killer_policy C)

add_executable(oom_policy_test shared/oom_policy.c tests/test_policy.c)
target_include_directories(oom_policy_test PRIVATE include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(oom_policy_test PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME oom_policy COMMAND oom_policy_test)
