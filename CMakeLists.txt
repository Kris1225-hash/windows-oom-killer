cmake_minimum_required(VERSION 3.16)
project(windows_oom_killer_policy C)

add_executable(oom_policy_test shared/oom_policy.c tests/test_policy.c)
target_include_directories(oom_policy_test PRIVATE include)
