# Manual profiler; instrumentation must not contaminate normal timing runs.
foreach instrumented : [false, true]
    suffix = instrumented ? '_counts' : ''
    executable('executor_bench' + suffix,
        'ExecutorBench.cpp',
        include_directories: inc,
        dependencies: common_deps,
        cpp_args: instrumented ? ['-DEXECUTOR_BENCH_COUNTS=1'] : [],
        link_args: instrumented ? ['-Wl,--wrap=malloc', '-Wl,--wrap=free'] : [],
        install: false)
endforeach
