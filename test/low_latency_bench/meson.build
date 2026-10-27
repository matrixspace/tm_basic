if get_option('buildtype') == 'debug'
    low_latency_bench_lib = tm_basic_lib_debug
else
    low_latency_bench_lib = tm_basic_lib
endif

#micro benchmark, not a test: run it by hand, pinned (see the header comment)
executable('low_latency_bench'
    , ['LowLatencyBench.cpp']
    , include_directories: inc
    , dependencies: [common_deps, dependency('date_tz')]
    , link_with: low_latency_bench_lib
    , install: false
)
