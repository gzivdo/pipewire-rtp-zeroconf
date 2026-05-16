common_sources = files(
  'avahi-poll.c',
  'peer-device.c',
)

common_inc = include_directories('.')

shared_library('pipewire-module-mdns-rtp-publish',
  ['module-mdns-rtp-publish.c'] + common_sources,
  include_directories : common_inc,
  dependencies : [ pipewire_dep, spa_dep, avahi_dep ],
  install : true,
  install_dir : pipewire_module_dir,
)

shared_library('pipewire-module-mdns-rtp-discover',
  ['module-mdns-rtp-discover.c'] + common_sources,
  include_directories : common_inc,
  dependencies : [ pipewire_dep, spa_dep, avahi_dep ],
  install : true,
  install_dir : pipewire_module_dir,
)
