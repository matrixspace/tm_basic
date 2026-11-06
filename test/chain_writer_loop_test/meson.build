if get_option('buildtype') == 'debug'
    chain_writer_loop_lib = tm_basic_lib_debug
else
    chain_writer_loop_lib = tm_basic_lib
endif
chain_writer_loop_exe = executable('chain_writer_loop_test'
    , ['ChainWriterLoopTest.cpp']
    , include_directories: inc
    , dependencies: common_deps
    , link_with: chain_writer_loop_lib
    , install: false
)
test('chain_writer_loop_test', chain_writer_loop_exe, timeout: 180)
