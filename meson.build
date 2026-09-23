project('pcble2gamepad', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=3', 'werror=true'])
gio = dependency('gio-unix-2.0', version: '>=2.74')
json = dependency('json-glib-1.0')
bluetooth = dependency('bluez')
libexec_dir = get_option('libexecdir') / 'pcble2gamepad'
configuration = configuration_data()
configuration.set_quoted('PCBLE2GAMEPAD_PRO_RUNNER', get_option('prefix') / libexec_dir / 'run-classic.sh')
configure_file(output: 'config.h', configuration: configuration)
executable('pcble2gamepaddiag', 'src/adapter-diag.c', dependencies: [gio, bluetooth], install: true)
inc = include_directories('src')
protocol = static_library('joycon2-protocol', 'src/protocol.c', dependencies: gio)
client = static_library('joycon2-client', 'src/client.c', dependencies: [gio, json])
daemon = executable('pcble2gamepadd', ['src/daemon.c', 'src/core.c', 'src/bluez.c', 'src/ipc.c', 'src/ipc-engine.c'],
  link_with: [protocol, client], dependencies: [gio, json], install: true)
ctl = executable('pcble2gamepadctl', 'src/cli.c', link_with: client, dependencies: [gio, json], install: true)
executable('pcble2gamepad-pro-backend',
  ['poc/pro-controller.c', 'poc/protocol.c', 'poc/control.c', 'src/ipc.c'],
  dependencies: [gio, json, bluetooth], install: true, install_dir: libexec_dir)
install_data('poc/pro-controller.xml', install_dir: libexec_dir)
install_data('poc/run-classic.sh', install_dir: libexec_dir,
  install_mode: 'rwxr-xr-x')
gtk = dependency('gtk4', version: '>=4.8', required: get_option('gui'))
adw = dependency('libadwaita-1', version: '>=1.2', required: get_option('gui'))
if gtk.found() and adw.found()
  sdl = dependency('sdl2', version: '>=2.0.14')
  math = meson.get_compiler('c').find_library('m')
  executable('pcble2gamepad-ble-lab', 'src/gui-ble.c', link_with: client, dependencies: [gio, json, gtk, adw], install: true)
  executable('pcble2gamepad', ['src/gui.c', 'src/input-model.c'], link_with: client, dependencies: [gio, json, gtk, adw, sdl, math], install: true)
  test('input-model', executable('test-input-model', ['tests/test-input-model.c', 'src/input-model.c'],
    include_directories: inc, dependencies: [gio, sdl, math]))
  install_data('data/io.github.wozt.pcble2gamepad.desktop', install_dir: get_option('datadir') / 'applications')
endif
test('protocol', executable('test-protocol', 'tests/test-protocol.c', include_directories: inc, link_with: protocol, dependencies: gio))
test('ipc', find_program('tests/test-ipc.sh'), args: [meson.current_build_dir()], depends: [daemon, ctl], timeout: 30)
test('core', executable('test-core', ['tests/test-core.c', 'src/core.c', 'src/bluez.c'], include_directories: inc, link_with: protocol, dependencies: [gio, json]))
fake_bluez = executable('fake-bluez', 'tests/fake-bluez.c', dependencies: gio)
test('bluez-lifecycle', find_program('tests/test-bluez.sh'), args: [meson.current_build_dir()], depends: [daemon, ctl, fake_bluez], timeout: 30)
