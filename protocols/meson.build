wl_protocol_dir = dep_wayland_protocols.get_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner')

wayland_scanner_code = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.c',
	arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_header = generator(
	wayland_scanner,
	output: '@BASENAME@-protocol.h',
	arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

protocols = [
	wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
]

protocols_src = []
protocols_headers = []

foreach xml : protocols
	protocols_src += wayland_scanner_code.process(xml)
	protocols_headers += wayland_scanner_header.process(xml)
endforeach

lib_protocols = static_library(
	'protocols',
	protocols_src + protocols_headers,
	dependencies: [dep_wayland_client]
)

dep_protocols = declare_dependency(
	link_with: lib_protocols,
	sources: protocols_headers,
)
