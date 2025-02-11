wayland_protocols = dependency('wayland-protocols')
proto_dir = wayland_protocols.get_pkgconfig_variable('pkgdatadir')
wlscanner = find_program('wayland-scanner')
# This needs to be done better..
protos = [
	'wlr-screencopy-unstable-v1.xml',
	proto_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
	proto_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
	# This should be optional...
	proto_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
]
wl_gen_c = generator(wlscanner,
	output: '@BASENAME@.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
wl_gen_h = generator(wlscanner,
	output: '@BASENAME@.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'])
if protos.length() > 0
	proto_c = wl_gen_c.process(protos)
	proto_h = wl_gen_h.process(protos)
else
	proto_c = []
	proto_h = []
endif
