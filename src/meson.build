spice_gtk_include += [include_directories('.')]

#
# Source files for spice-client-glib
#

# generate spice-version.h
if meson.project_version().startswith('UNKNOWN')
  major = '0'
  minor = '0'
  micro = '1'
else
  version_info = meson.project_version().split('.')
  major = '@0@'.format(version_info[0])
  minor = '@0@'.format(version_info[1])
  micro = '0'
  if version_info.length() > 2
    micro = version_info[2].split('-')[0]
  endif
endif

version_data = configuration_data()
version_data.set('SPICE_GTK_MAJOR_VERSION', major)
version_data.set('SPICE_GTK_MINOR_VERSION', minor)
version_data.set('SPICE_GTK_MICRO_VERSION', micro)
spice_version_h = configure_file(input : 'spice-version.h.in',
                                 output : 'spice-version.h',
                                 configuration : version_data)

spice_client_glib_headers = [
  spice_version_h,
  'channel-cursor.h',
  'channel-display.h',
  'channel-inputs.h',
  'channel-main.h',
  'channel-playback.h',
  'channel-port.h',
  'channel-record.h',
  'channel-smartcard.h',
  'channel-usbredir.h',
  'channel-webdav.h',
  'qmp-port.h',
  'smartcard-manager.h',
  'spice-audio.h',
  'spice-channel.h',
  'spice-client.h',
  'spice-file-transfer-task.h',
  'spice-option.h',
  'spice-session.h',
  'spice-types.h',
  'spice-uri.h',
  'spice-util.h',
  'usb-device-manager.h',
]

install_headers(spice_client_glib_headers, subdir : 'spice-client-glib-2.0')

# generate spice-marshal.[ch]
gnome = import('gnome')
spice_marshals = gnome.genmarshal('spice-marshal', sources : 'spice-marshal.txt')

# generate spice-glib-enums.[ch]
spice_client_glib_enums = gnome.mkenums_simple('spice-glib-enums',
                                               sources : ['spice-channel.h', 'channel-inputs.h', 'spice-session.h'],
                                               install_header : true,
                                               install_dir : spice_gtk_includedir / 'spice-client-glib-2.0')

spice_client_glib_introspection_sources = [
  spice_client_glib_headers,
  spice_client_glib_enums,
  'channel-cursor.c',
  'channel-display.c',
  'channel-inputs.c',
  'channel-main.c',
  'channel-playback.c',
  'channel-port.c',
  'channel-record.c',
  'channel-smartcard.c',
  'channel-usbredir.c',
  'channel-webdav.c',
  'qmp-port.c',
  'smartcard-manager.c',
  'spice-audio.c',
  'spice-channel.c',
  'spice-client.c',
  'spice-option.c',
  'spice-session.c',
  'spice-util.c',
  'usb-device-manager.c',
]

spice_client_glib_sources = [
  spice_marshals,
  spice_client_glib_introspection_sources,
  'bio-gio.c',
  'bio-gio.h',
  'channel-base.c',
  'channel-display-gst.c',
  'channel-display-priv.h',
  'channel-playback-priv.h',
  'channel-usbredir-priv.h',
  'client_sw_canvas.c',
  'client_sw_canvas.h',
  'coroutine.h',
  'decode-glz.c',
  'decode.h',
  'decode-jpeg.c',
  'decode-zlib.c',
  'gio-coroutine.c',
  'gio-coroutine.h',
  'qmp-port.c',
  'qmp-port.h',
  'smartcard-manager-priv.h',
  'spice-audio-priv.h',
  'spice-channel-cache.h',
  'spice-channel-priv.h',
  'spice-common.h',
  'spice-file-transfer-task.c',
  'spice-file-transfer-task-priv.h',
  'spice-glib-main.c',
  'spice-gstaudio.c',
  'spice-gstaudio.h',
  'spice-option.h',
  'spice-session-priv.h',
  'spice-uri.c',
  'spice-uri-priv.h',
  'spice-util-priv.h',
  'usb-device-manager-priv.h',
  'vmcstream.c',
  'vmcstream.h',
]

if spice_gtk_has_builtin_mjpeg
  spice_client_glib_sources += 'channel-display-mjpeg.c'
endif

if spice_gtk_has_polkit
  spice_client_glib_sources += ['usb-acl-helper.c',
                                'usb-acl-helper.h']
endif

if spice_gtk_has_phodav
  spice_client_glib_sources += ['giopipe.c',
                                'giopipe.h']
endif

if spice_gtk_coroutine == 'gthread'
  spice_client_glib_sources += 'coroutine_gthread.c'
elif spice_gtk_coroutine in ['ucontext', 'libucontext']
  spice_client_glib_sources += ['continuation.c',
                                'continuation.h',
                                'coroutine_ucontext.c']
elif spice_gtk_coroutine == 'winfiber'
  spice_client_glib_sources += 'coroutine_winfibers.c'
endif

if spice_gtk_has_usbredir
  spice_client_glib_sources += [
    'usbutil.c',
    'usbutil.h',
    'usb-backend.c',
    'usb-emulation.h',
    'usb-backend.h',
    'usb-device-cd.c',
    'usb-device-cd.h',
    'cd-scsi.c',
    'cd-scsi.h',
    'cd-scsi-dev-params.h',
    'cd-usb-bulk-msd.c',
    'cd-usb-bulk-msd.h',
  ]
endif

if spice_gtk_has_usbredir and host_machine.system() == 'windows'
  spice_client_glib_sources += ['usbdk_api.c',
                                'usbdk_api.h']
endif

#
# libspice-client-glib-2.0.so
#

# version-script
spice_client_glib_syms = files('map-file')
spice_client_glib_syms_path = meson.current_source_dir() / 'map-file'
spice_gtk_version_script = '-Wl,--version-script=@0@'.format(spice_client_glib_syms_path)
spice_gtk_has_version_script = compiler.has_link_argument(spice_gtk_version_script)
if not spice_gtk_has_version_script
  spice_client_glib_syms = files('spice-glib-sym-file')
  spice_client_glib_syms_path = meson.current_source_dir() / 'spice-glib-sym-file'
  spice_gtk_version_script = ['-export-symbols', spice_client_glib_syms_path]
endif

# soversion
# http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
spice_client_glib_current = 16
spice_client_glib_revision = 0
spice_client_glib_age = 8
spice_client_glib_so_version = '@0@.@1@.@2@'.format(spice_client_glib_current - spice_client_glib_age,
                                                    spice_client_glib_age,
                                                    spice_client_glib_revision)
summary_info += {'libspice-client-glib.so version': spice_client_glib_so_version}

spice_client_glib_lib = library('spice-client-glib-2.0', spice_client_glib_sources,
                                version : spice_client_glib_so_version,
                                install : true,
                                include_directories : spice_gtk_include,
                                link_args : [spice_gtk_version_script],
                                link_depends : spice_client_glib_syms,
                                dependencies : spice_glib_deps)

spice_client_glib_dep = declare_dependency(sources : [spice_marshals[1], spice_client_glib_enums[1]],
                                           link_with : spice_client_glib_lib,
                                           include_directories : spice_gtk_include,
                                           dependencies : spice_glib_deps)

#
# SpiceClientGLib-2.0.gir
#
if spice_gtk_has_introspection
spice_client_glib_gir = gnome.generate_gir(spice_client_glib_lib,
                                           export_packages : 'spice-client-glib-2.0',
                                           extra_args : ['--accept-unprefixed'],
                                           header : 'spice-client.h',
                                           includes : ['GObject-2.0', 'Gio-2.0', 'GstBase-1.0'],
                                           identifier_prefix : 'Spice',
                                           include_directories: spice_gtk_include,
                                           link_with : spice_client_glib_lib,
                                           symbol_prefix : 'spice',
                                           install : spice_gtk_has_introspection,
                                           namespace : 'SpiceClientGLib',
                                           nsversion : '2.0',
                                           sources : spice_client_glib_introspection_sources)
endif
#
# spice-client-glib-usb-acl-helper
#
if spice_gtk_has_polkit
  executable('spice-client-glib-usb-acl-helper',
             'spice-client-glib-usb-acl-helper.c',
             include_directories : spice_gtk_include,
             install : true,
             install_dir : spice_gtk_usb_acl_helper_dir,
             dependencies : spice_acl_deps,
             pie : spice_gtk_has_pie
            )
  meson.add_install_script('../build-aux/setcap-or-suid', spice_gtk_usb_acl_helper_dir / 'spice-client-glib-usb-acl-helper', 'cap_fowner+ep')
endif


if spice_gtk_has_gtk
  #
  # Source files for spice-client-gtk
  #

  spice_client_gtk_headers = [
    'spice-client-gtk.h',
    'spice-grabsequence.h',
    'spice-gtk-session.h',
    'spice-widget.h',
    'usb-device-widget.h',
  ]

  install_headers(spice_client_gtk_headers, subdir : 'spice-client-gtk-3.0')

  # generate spice-widget-enums.[ch]
  spice_widget_enums = gnome.mkenums_simple('spice-widget-enums',
                                            sources : 'spice-widget.h',
                                            install_header : true,
                                            install_dir : spice_gtk_includedir / 'spice-client-gtk-3.0')

  spice_client_gtk_introspection_sources = [
    spice_client_gtk_headers,
    spice_widget_enums,
    'spice-grabsequence.c',
    'spice-gtk-session.c',
    'spice-widget.c',
    'usb-device-widget.c',
  ]

  spice_client_gtk_sources = [
    spice_marshals,
    spice_client_gtk_introspection_sources,
    'desktop-integration.c',
    'desktop-integration.h',
    'spice-file-transfer-task.h',
    'spice-grabsequence.h',
    'spice-grabsequence-priv.h',
    'spice-gtk-session-priv.h',
    'spice-util.c',
    'spice-util-priv.h',
    'spice-widget-cairo.c',
    'spice-widget-priv.h',
    'vncdisplaykeymap.c',
    'vncdisplaykeymap.h',
  ]

  if spice_gtk_has_egl
    spice_client_gtk_sources += 'spice-widget-egl.c'
  endif

  # keymaps
  python = import('python').find_installation()
  keymaps = ['xorgevdev',
             'xorgkbd',
             'xorgxquartz',
             'xorgxwin',
             'osx',
             'win32',
             'x11']

  foreach keymap : keymaps
    varname = 'keymap_@0@2xtkbd'.format(keymap)
    target = 'vncdisplay@0@.h'.format(varname)
    cmd = [python, keymapgen, '--lang', 'glib2', '--varname', varname, 'code-map', keymapcsv, keymap, 'xtkbd']
    spice_client_gtk_sources += custom_target(target,
                                              output : target,
                                              capture : true,
                                              build_by_default: true,
                                              command : cmd)
  endforeach

  #
  # Wayland protocols
  #
  if spice_gtk_has_wayland_protocols

    generated_protocols = [
      [ 'pointer-constraints', 'v1' ],
      [ 'relative-pointer', 'v1' ],
     #[ 'input-method', 'v1' ],
     #[ 'input-timestamps', 'v1' ],
     #[ 'fullscreen-shell', 'v1' ],
     #[ 'linux-dmabuf', 'v1' ],
     #[ 'linux-explicit-synchronization', 'v1' ],
     #[ 'presentation-time', 'stable' ],
     #[ 'tablet', 'v2' ],
     #[ 'text-input', 'v1' ],
     #[ 'viewporter', 'stable' ],
     #[ 'xdg-output', 'v1' ],
     #[ 'xdg-shell', 'v6' ],
     #[ 'xdg-shell', 'stable' ],
    ]

    foreach proto: generated_protocols
      proto_name = proto[0]
      if proto[1] == 'internal'
        base_file = proto_name
        xml_path = '@0@.xml'.format(proto_name)
      elif proto[1] == 'stable'
        base_file = proto_name
        xml_path = '@0@/stable/@1@/@1@.xml'.format(dir_wp_base, base_file)
      else
        base_file = '@0@-unstable-@1@'.format(proto_name, proto[1])
        xml_path = '@0@/unstable/@1@/@2@.xml'.format(dir_wp_base, proto_name, base_file)
      endif

      foreach output_type: [ 'client-header', 'server-header', 'private-code' ]
        if output_type == 'client-header'
          output_file = '@0@-client-protocol.h'.format(base_file)
        elif output_type == 'server-header'
          output_file = '@0@-server-protocol.h'.format(base_file)
        else
          output_file = '@0@-protocol.c'.format(base_file)
          if dep_scanner.version().version_compare('< 1.14.91')
              output_type = 'code'
          endif
        endif

        spice_client_gtk_sources += custom_target(
          '@0@ @1@'.format(base_file, output_type),
          command: [ prog_scanner, output_type, '@INPUT@', '@OUTPUT@' ],
          input: xml_path,
          output: output_file,
        )
      endforeach
    endforeach

    spice_client_gtk_sources += [
      'wayland-extensions.c',
      'wayland-extensions.h',
    ]
  endif

  #
  # libspice-client-gtk.so
  #
  spice_client_gtk_syms = spice_client_glib_syms
  if not spice_gtk_has_version_script
    spice_client_gtk_syms = files('spice-gtk-sym-file')
    spice_client_gtk_syms_path = meson.current_source_dir() / 'spice-gtk-sym-file'
    spice_gtk_version_script = ['-export-symbols', spice_client_gtk_syms_path]
  endif

  # soversion
  # http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
  spice_client_gtk_current = 6
  spice_client_gtk_revision = 0
  spice_client_gtk_age = 1
  spice_client_gtk_so_version = '@0@.@1@.@2@'.format(spice_client_gtk_current - spice_client_gtk_age,
                                                     spice_client_gtk_age,
                                                     spice_client_gtk_revision)
  summary_info += {'libspice-client-gtk.so version': spice_client_gtk_so_version}

  spice_client_gtk_lib = library('spice-client-gtk-3.0', spice_client_gtk_sources,
                                 version : spice_client_gtk_so_version,
                                 install : true,
                                 link_args : [spice_gtk_version_script],
                                 link_depends : spice_client_gtk_syms,
                                 dependencies : [spice_client_glib_dep, spice_gtk_deps, spice_wayland_deps])

  spice_client_gtk_dep = declare_dependency(sources : spice_widget_enums[1],
                                            link_with : spice_client_gtk_lib,
                                            dependencies : [spice_client_glib_dep, spice_gtk_deps, spice_wayland_deps])

  if spice_gtk_has_introspection
    #
    # SpiceClientGtk-3.0.gir
    #
    spice_client_gtk_gir = gnome.generate_gir(spice_client_gtk_lib,
                                              export_packages : 'spice-client-gtk-3.0',
                                              extra_args : ['--accept-unprefixed'],
                                              header : 'spice-widget.h',
                                              includes : ['GObject-2.0', 'Gtk-3.0', spice_client_glib_gir[0]],
                                              include_directories: spice_gtk_include,
                                              link_with : spice_client_glib_lib,
                                              identifier_prefix : 'Spice',
                                              symbol_prefix : 'spice',
                                              install : spice_gtk_has_introspection,
                                              namespace : 'SpiceClientGtk',
                                              nsversion : '3.0',
                                              sources : spice_client_gtk_introspection_sources)
  endif
endif
