if get_option('buildtype') == 'debug'
    hwloc_cpu_allocator_test_lib = tm_basic_lib_debug
else
    hwloc_cpu_allocator_test_lib = tm_basic_lib
endif

hwloc_cpu_allocator_test_exe = executable('hwloc_cpu_allocator_test', 'HwlocCpuAllocatorTest.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: hwloc_cpu_allocator_test_lib,
    install: false)
test('hwloc_cpu_allocator_test', hwloc_cpu_allocator_test_exe, timeout: 120)

hwloc_cpu_allocator_constraints_exe = executable('hwloc_cpu_allocator_constraints', 'HwlocCpuAllocatorConstraints.cpp',
    include_directories: inc,
    dependencies: common_deps,
    link_with: hwloc_cpu_allocator_test_lib,
    install: false)
test('hwloc_cpu_allocator_constraints', find_program('run_constraints.sh'),
    args: [hwloc_cpu_allocator_constraints_exe.full_path()], timeout: 180)
