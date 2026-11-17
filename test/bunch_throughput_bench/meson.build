if get_option('buildtype') == 'debug'
    bunch_throughput_bench_lib = tm_basic_lib_debug
else
    bunch_throughput_bench_lib = tm_basic_lib
endif

#benchmark, not a test: run it by hand, pinned to idle cores
executable('bunch_throughput_bench'
    , ['BunchThroughputBench.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: bunch_throughput_bench_lib
    , install: false
)
