if get_option('buildtype') == 'debug'
    cpu_bind_string_basic_test_lib = tm_basic_lib_debug
else
    cpu_bind_string_basic_test_lib = tm_basic_lib
endif

cpu_bind_string_basic_test_exe = executable('cpu_bind_string_basic_test', 'CpuBindStringBasicTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: cpu_bind_string_basic_test_lib,
    install: false)
test('cpu_bind_string_basic_test', cpu_bind_string_basic_test_exe, timeout: 30)
