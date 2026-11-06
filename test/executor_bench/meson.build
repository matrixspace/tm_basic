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

# Compile each backend independently for comparisons: unrelated executor
# template changes must not affect shared callback inlining decisions.
engine_names = ['direct', 'inline', 'inline_erased', 'dedicated', 'pool', 'moody']
foreach index : range(engine_names.length())
    foreach instrumented : [false, true]
        suffix = instrumented ? '_counts' : ''
        executable('executor_bench_' + engine_names[index] + suffix,
            'ExecutorBench.cpp',
            include_directories: inc,
            dependencies: common_deps,
            cpp_args: ['-DEXECUTOR_BENCH_ENGINE=' + (index+1).to_string()] + (instrumented ? ['-DEXECUTOR_BENCH_COUNTS=1'] : []),
            link_args: instrumented ? ['-Wl,--wrap=malloc', '-Wl,--wrap=free'] : [],
            build_by_default: false,
            install: false)
    endforeach
endforeach

executable('executor_latency_bench', 'ExecutorLatencyBench.cpp',
    include_directories: inc,
    dependencies: common_deps,
    build_by_default: false,
    install: false)
