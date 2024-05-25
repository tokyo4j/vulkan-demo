project(
  'vulkan-demo',
  'c',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments(meson.get_compiler('c').get_supported_arguments([
  '-Wno-unused-parameter',
]), language: 'c')

dep_wayland_protocols = dependency('wayland-protocols')
dep_wayland_client = dependency('wayland-client')
dep_vulkan = dependency('vulkan')

subdir('protocols')
subdir('shaders')
subdir('src')

executable(
  meson.project_name(),
  sources,
  dependencies: [
    dep_wayland_protocols,
    dep_wayland_client,
    dep_vulkan,
    dep_protocols,
    dep_shaders,
  ]
)
