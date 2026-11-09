if build_machine.system() == 'linux'
    subdir('tsc_clock_test')
    subdir('low_latency_bench')
    if have_moodycamel_executor
        subdir('moodycamel_executor_test')
    endif
    subdir('simple_graph_bench')
    subdir('chain_writer_loop_test')
    subdir('executor_bench')
endif

subdir('shared_pool_executor_test')
