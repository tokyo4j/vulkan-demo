glslang = generator(
  find_program('glslangValidator'),
  output: '@PLAINNAME@.spv',
  arguments: ['@INPUT@', '--quiet', '-V', '-x', '-o', '@OUTPUT@']
)

spirv_files = glslang.process('triangle.vert', 'triangle.frag')

dep_shaders = declare_dependency(
  sources: spirv_files,
)
