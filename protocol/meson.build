wlscanner = find_program('wayland-scanner')
# This needs to be done better..
protos = [
	'wlr-screencopy-unstable-v1.xml'
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
