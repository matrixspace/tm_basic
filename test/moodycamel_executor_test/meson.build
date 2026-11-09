if get_option('buildtype') == 'debug'
    moodycamel_executor_test_lib = tm_basic_lib_debug
else
    moodycamel_executor_test_lib = tm_basic_lib
endif

#moodycamel (vcpkg port "concurrentqueue") is header-only and lives under the
#vcpkg include root that common_deps already puts on the include path
moodycamel_executor_test_exe = executable('moodycamel_executor_test'
    , ['MoodycamelExecutorTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: moodycamel_executor_test_lib
    , install: false
)
test('moodycamel_executor_test', moodycamel_executor_test_exe, timeout: 180)

moodycamel_queue_test = executable('moodycamel_queue_test', 'MoodycamelQueueTest.cpp',
    include_directories: inc, dependencies: common_deps, install: false)
test('moodycamel_queue_test', moodycamel_queue_test, timeout: 120)
