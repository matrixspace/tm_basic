enhanced_test = executable('enhanced_dedicated_executor_test', 'EnhancedDedicatedExecutorTest.cpp',
    include_directories: inc, dependencies: common_deps, install: false)
test('enhanced_dedicated_executor_test', enhanced_test, timeout: 120)

enhanced_handler_test = executable('enhanced_handler_test',
    '../moodycamel_executor_test/MoodycamelExecutorTest.cpp',
    include_directories: inc, dependencies: common_deps,
    cpp_args: ['-DTM_KIT_BASIC_TEST_ENHANCED_EXECUTOR=1'], install: false)
test('enhanced_handler_test', enhanced_handler_test, timeout: 180)
