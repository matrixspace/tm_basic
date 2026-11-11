project('TM kit basic', 'cpp', default_options: ['cpp_std=c++17', 'warning_level=3'])

inc = include_directories('.')
thread_dep = dependency('threads')
boost_modules = ['log', 'log_setup', 'thread', 'filesystem', 'iostreams']

# Try adding 'system'
boost_dep = dependency('boost',
  modules : boost_modules + ['system'],
  required : false
)

# If Boost with 'system' not found, retry without it
if not boost_dep.found()
  boost_dep = dependency('boost',
    modules : boost_modules,
    required : true
  )
endif

spd_dep = dependency('spdlog')
if get_option('buildtype') == 'debug'
  tm_infra_dep = dependency('tm_kit_infra_debug')
else
  tm_infra_dep = dependency('tm_kit_infra')
endif
nlohmann_json_dep = dependency('nlohmann-json')
simd_json_dep = dependency('simdjson')
common_deps = [thread_dep, boost_dep, spd_dep, tm_infra_dep, nlohmann_json_dep, simd_json_dep]

moodycamel_option = get_option('moodycamel_executor')
have_moodycamel_executor = false
if not moodycamel_option.disabled()
  cpp_compiler = meson.get_compiler('cpp')
  have_moodycamel_executor = cpp_compiler.has_header('concurrentqueue/moodycamel/concurrentqueue.h', dependencies: common_deps) or cpp_compiler.has_header('moodycamel/concurrentqueue.h', dependencies: common_deps)
endif
if moodycamel_option.enabled() and not have_moodycamel_executor
  error('moodycamel_executor enabled but concurrentqueue headers are unavailable')
endif
if not have_moodycamel_executor
  add_project_arguments('-DTM_KIT_BASIC_DISABLE_MOODYCAMEL_THREADED_EXECUTOR=1', language: 'cpp')
endif
message('Optional moodycamel executor: ' + have_moodycamel_executor.to_string())

hwloc_option = get_option('hwloc_cpu_allocator')
have_hwloc_cpu_allocator = false
if not hwloc_option.disabled()
  hwloc_dep = dependency('hwloc', required: hwloc_option.enabled())
  have_hwloc_cpu_allocator = hwloc_dep.found()
  if have_hwloc_cpu_allocator
    common_deps += hwloc_dep
  endif
endif
if not have_hwloc_cpu_allocator
  add_project_arguments('-DTM_KIT_BASIC_DISABLE_HWLOC_CPU_ALLOCATOR=1', language: 'cpp')
endif
message('Optional hwloc CPU allocator: ' + have_hwloc_cpu_allocator.to_string())


if build_machine.system() == 'windows'
  add_global_arguments('-DWINVER=0x0601', language: 'cpp')
  add_global_arguments('-D_WIN32_WINNT=0x0601', language: 'cpp')
  add_global_arguments('-DBOOST_USE_WINAPI_VERSION=BOOST_WINAPI_VERSION_WIN7', language: 'cpp')
endif

if build_machine.system() == 'linux'
    add_global_arguments('-Wno-unknown-pragmas', language: 'cpp')
    add_global_arguments('-Wno-unused-function', language: 'cpp')
    add_global_arguments('-Wno-reorder', language: 'cpp')
    if get_option('buildtype') != 'release'
        add_global_arguments('-fno-omit-frame-pointer', language: 'cpp')
        add_global_arguments('-fno-omit-frame-pointer', language: 'c')
    endif
endif

pkg = import('pkgconfig')

subdir('tm_kit/basic')
subdir('src')
subdir('test')
