foreach entry : [['threaded_handler_test', 'ThreadedHandlerTest.cpp'], ['executor_mechanism_test', 'ExecutorMechanismTest.cpp']]
    exe = executable(entry[0], entry[1], include_directories: inc, dependencies: common_deps, install: false)
    test(entry[0], exe, timeout: 300)
endforeach
