if get_option('buildtype') == 'debug'
    common_flow_utils_cpu_bind_test_lib = tm_basic_lib_debug
else
    common_flow_utils_cpu_bind_test_lib = tm_basic_lib
endif

common_flow_utils_cpu_bind_test_exe = executable('common_flow_utils_cpu_bind_test', 'CommonFlowUtilsCpuBindTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: common_flow_utils_cpu_bind_test_lib,
    install: false)
test('common_flow_utils_cpu_bind_test', common_flow_utils_cpu_bind_test_exe, timeout: 120)
