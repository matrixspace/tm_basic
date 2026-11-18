if get_option('buildtype') == 'debug'
    realtime_shutdown_lib = tm_basic_lib_debug
else
    realtime_shutdown_lib = tm_basic_lib
endif
realtime_shutdown_exe = executable('realtime_shutdown_test'
    , ['RealTimeShutdownTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: realtime_shutdown_lib
    , install: false
)
test('realtime_shutdown_test', realtime_shutdown_exe, timeout: 30)
