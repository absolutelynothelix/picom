# enable xrender
srcs += [
  files(
    'dummy/dummy.c',
    'backend.c',
    'backend_common.c',
    'driver.c',
  ),
]
subdir('xrender')
# enable opengl
if get_option('opengl')
  subdir('gl')
endif

# enable vulkan
if get_option('vulkan')
  srcs += [ files('vulkan/vulkan.c') ]
endif
