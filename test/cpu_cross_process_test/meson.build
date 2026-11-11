if get_option('buildtype') == 'debug'
    cpu_cross_process_test_lib = tm_basic_lib_debug
else
    cpu_cross_process_test_lib = tm_basic_lib
endif

cpu_cross_process_test_exe = executable('cpu_cross_process_test', 'CrossProcessCpuTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: cpu_cross_process_test_lib,
    install: false)
test('cpu_cross_process_test', find_program('run_cross_process.sh'),
    args: [cpu_cross_process_test_exe.full_path()], timeout: 300, is_parallel: false)
