project('slorp', 'c',
	default_options: [
		'c_std=c11'
	],
	version: '0.1'
)
#includes = include_directories(['include'])
wayland_client = dependency('wayland-client')
cairo = dependency('cairo')
pixman = dependency('pixman-1')
png = dependency('libpng')
want_jxl = get_option('libjxl')
jxl = dependency('libjxl', required: want_jxl)
# nwl must be a subproject for now :)
nwlsub = subproject('nwl', required: true)
if nwlsub.found()
	nwl = nwlsub.get_variable('nwl_dep')
else
	nwl = dependency('nwl', required: true)
endif
subdir('protocol')
srcfiles = [
	'main.c'
]
deps = [
	wayland_client,
	cairo,
	nwl,
	pixman,
	png
]
conf_data = configuration_data()
if jxl.found()
	deps += jxl
	conf_data.set('HAVE_JXL', 1)
endif
configure_file(output: 'config.h', configuration:conf_data)
executable('slorp', srcfiles, [proto_c, proto_h], dependencies: deps, install:true)
