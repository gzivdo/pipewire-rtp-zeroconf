project('pipewire-net-zeroconf', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>= 0.59.0',
  default_options : [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

pipewire_dep = dependency('libpipewire-0.3', version : '>= 0.3.60')
spa_dep      = dependency('libspa-0.2',     version : '>= 0.2')
avahi_dep    = dependency('avahi-client')

pipewire_module_dir = pipewire_dep.get_variable(
  pkgconfig : 'moduledir',
  default_value : get_option('libdir') / 'pipewire-0.3'
)

pipewire_conf_dir = get_option('datadir') / 'pipewire' / 'pipewire.conf.d'

add_project_arguments(
  '-D_GNU_SOURCE',
  '-DPACKAGE_VERSION="' + meson.project_version() + '"',
  language : 'c',
)

subdir('src')
subdir('conf')

summary({
  'PipeWire module dir' : pipewire_module_dir,
  'PipeWire conf dir'   : pipewire_conf_dir,
})
