if get_option('buildtype') == 'debug'
    tsc_clock_test_lib = tm_basic_lib_debug
else
    tsc_clock_test_lib = tm_basic_lib
endif

tsc_clock_test_exe = executable('tsc_clock_test'
    , ['TscClockTest.cpp']
    , include_directories: inc
    , dependencies: [common_deps, dependency('date_tz')]
    , link_with: tsc_clock_test_lib
    , install: false
)
test('tsc_clock_test', tsc_clock_test_exe, timeout: 120)
