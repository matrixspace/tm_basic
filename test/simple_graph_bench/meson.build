if get_option('buildtype') == 'debug'
    simple_graph_bench_lib = tm_basic_lib_debug
else
    simple_graph_bench_lib = tm_basic_lib
endif

#benchmark, not a test: run it by hand, pinned to idle cores
executable('simple_graph_bench'
    , ['SimpleGraphBench.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: simple_graph_bench_lib
    , install: false
)
