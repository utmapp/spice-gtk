#
# project definition
#
project('spice-gtk', 'c',
         version : run_command('build-aux/git-version-gen', '@0@/.tarball-version'.format(meson.source_root()), check : true).stdout().strip(),
         license : 'LGPLv2.1',
         meson_version : '>= 0.53',
         default_options : ['buildtype=debugoptimized',
                            'warning_level=2'])

meson.add_dist_script('build-aux/meson-dist', meson.project_version(), meson.source_root())
summary_info = {}

#
# global C defines
#
summary_info = {'prefix': get_option('prefix')}
spice_gtk_prefix = get_option('prefix')
spice_gtk_bindir = spice_gtk_prefix / get_option('bindir')
spice_gtk_datadir = spice_gtk_prefix / get_option('datadir')
spice_gtk_localedir = spice_gtk_datadir / 'locale'
spice_gtk_includedir = spice_gtk_prefix / get_option('includedir')
spice_gtk_global_cflags = ['-DHAVE_CONFIG_H',
                           '-DSPICE_COMPILATION',
                           '-DG_LOG_DOMAIN="GSpice"',
                           '-Wno-sign-compare',
                           '-Wno-unused-parameter',
                           '-Wno-cast-function-type']

# other global vars
compiler = meson.get_compiler('c')
spice_gtk_config_data = configuration_data()
spice_gtk_include = [include_directories('.')]
spice_glib_deps = []
spice_gtk_deps = []
spice_wayland_deps = []
spice_acl_deps = []
spice_protocol_version = '0.14.3'

#
# Set up subprojects
#
spice_common = subproject('spice-common',
                          default_options : [
                            'generate-code=client',
                            'spice-protocol-version=@0@'.format(spice_protocol_version),
                          ])
spice_gtk_config_data.merge_from(spice_common.get_variable('spice_common_config_data'))
spice_glib_deps += spice_common.get_variable('spice_common_client_dep')
spice_protocol_version = spice_common.get_variable('spice_protocol_version')

keycodemapdb = subproject('keycodemapdb')

keymapgen = find_program('keymap-gen')
keymapcsv = keycodemapdb.get_variable('keymaps_csv')

#
# check for system headers
#
headers = [
  'termios.h',
  'X11/XKBlib.h',
  'sys/socket.h',
  'sys/types.h',
  'netinet/in.h',
  'arpa/inet.h',
  'valgrind/valgrind.h',
  'sys/disk.h'
]

foreach header : headers
  if compiler.has_header(header)
    spice_gtk_config_data.set('HAVE_@0@'.format(header.underscorify().to_upper()), '1')
  endif
endforeach

# FIXME: Add a config option to validate spice-widget-egl being built or not
spice_gtk_has_egl = compiler.has_header('epoxy/egl.h')
spice_gtk_config_data.set('HAVE_EGL', spice_gtk_has_egl)

#
# check for system functions
#
foreach func : ['clearenv', 'strtok_r']
  if compiler.has_function(func)
    spice_gtk_config_data.set('HAVE_@0@'.format(func.underscorify().to_upper()), '1')
  endif
endforeach

#
# check for mandatory dependencies
#
glib_version = '2.52'
glib_version_info = '>= @0@'.format(glib_version)
pixman_version = '>= 0.17.7'

deps = {'spice-protocol' : '>= @0@'.format(spice_protocol_version),
        'glib-2.0'       : glib_version_info,
        'gio-2.0'        : glib_version_info,
        'gobject-2.0'    : glib_version_info,
        'pixman-1'       : pixman_version,
        'openssl'        : '>= 1.0.0'}

foreach dep, version : deps
  spice_glib_deps += dependency(dep, version : version)
endforeach

# mandatory dependencies, without specific version requirement
# TODO: specify minimum version for jpeg and zlib?
deps = ['libjpeg', 'zlib', 'json-glib-1.0']
if host_machine.system() == 'windows'
  deps += 'gio-windows-2.0'
else
  deps += 'gio-unix-2.0'
endif

foreach dep : deps
  spice_glib_deps += dependency(dep)
endforeach

deps = []
if host_machine.system() == 'windows'
  deps += ['libws2_32', 'libgdi32', 'libcomctl32']
endif

foreach dep : deps
  spice_glib_deps += compiler.find_library(dep)
endforeach

#
# Non-mandatory/optional dependencies
#
optional_deps = {'opus' : '>= 0.9.14'}
foreach dep, version : optional_deps
  d = dependency(dep, required : get_option(dep), version : version)
  summary_info += {dep: d.found()}
  if d.found()
    spice_glib_deps += d
    spice_gtk_config_data.set('HAVE_@0@'.format(dep.underscorify().to_upper()), '1')
  endif
endforeach

# gtk
spice_gtk_has_gtk = false
gtk_version_required = '3.22'
d = dependency('gtk+-3.0', version : '>= @0@'.format(gtk_version_required),
               required: get_option('gtk'))
summary_info += {'gtk': d.found()}
if d.found()
  spice_gtk_deps += d
  if host_machine.system() != 'windows'
    spice_gtk_deps += dependency('epoxy')
    d = dependency('x11', required: false)
    if d.found()
      spice_gtk_deps += d
    endif
    d = dependency('libva-x11', required: false)
    if d.found()
      spice_gtk_deps += d
      spice_gtk_config_data.set('HAVE_LIBVA', '1')
    endif
  endif
  spice_gtk_has_gtk = true
endif

# wayland protocols
spice_gtk_has_wayland_protocols = false
# Check if gtk is enabled and supports the wayland backend
if host_machine.system() != 'windows' and spice_gtk_has_gtk and compiler.has_header('gtk-3.0/gdk/gdkwayland.h')
  d = dependency('wayland-protocols', version: '>= 1.17', required: get_option('wayland-protocols'))
  summary_info += {'wayland-protocols': d.found()}
  if d.found()
    spice_gtk_config_data.set('HAVE_WAYLAND_PROTOCOLS', '1')
    dir_wp_base = d.get_pkgconfig_variable('pkgdatadir')
    dep_scanner = dependency('wayland-scanner', native: true)
    prog_scanner = find_program(dep_scanner.get_pkgconfig_variable('wayland_scanner'))

    wayland_libs_version_required = '1.17.0'
    spice_wayland_deps += dependency('wayland-server', version : '>= @0@'.format(wayland_libs_version_required))
    spice_wayland_deps += dependency('wayland-cursor', version : '>= @0@'.format(wayland_libs_version_required))
    spice_wayland_deps += dependency('wayland-client', version : '>= @0@'.format(wayland_libs_version_required))
    spice_gtk_has_wayland_protocols = true
  endif
endif

# webdav
spice_gtk_has_phodav = false
phodav_dep = dependency('libphodav-3.0', required: false)
if not phodav_dep.found()
  phodav_dep = dependency('libphodav-2.0', required: get_option('webdav'))
endif
if phodav_dep.found()
  spice_glib_deps += phodav_dep
  if phodav_dep.name() == 'libphodav-3.0'
    d = dependency('libsoup-3.0', version : '>= 3.0', required: get_option('webdav'))
  else
    d = dependency('libsoup-2.4', version : '>= 2.49.91', required: get_option('webdav'))
  endif
  if d.found()
    spice_glib_deps += d
    spice_gtk_config_data.set('USE_PHODAV', '1')
    spice_gtk_has_phodav = true
    if phodav_dep.version().version_compare('>= 2.5')
      spice_gtk_config_data.set('HAVE_PHODAV_VIRTUAL', '1')
    endif
  endif
endif
summary_info += {'webdav': spice_gtk_has_phodav}

gstreamer_version = '1.10'
gstreamer_version_info = '>= @0@'.format(gstreamer_version)
deps = ['gstreamer-1.0', 'gstreamer-base-1.0', 'gstreamer-app-1.0', 'gstreamer-audio-1.0', 'gstreamer-video-1.0']
foreach dep : deps
  spice_glib_deps += dependency(dep, version: gstreamer_version_info)
endforeach

# builtin-mjpeg
spice_gtk_has_builtin_mjpeg = false
if get_option('builtin-mjpeg')
  spice_gtk_config_data.set('HAVE_BUILTIN_MJPEG', '1')
  spice_gtk_has_builtin_mjpeg = true
endif
summary_info += {'builtin-mjpeg': get_option('builtin-mjpeg')}

# usbredir
spice_gtk_has_usbredir = false
usbredir_version = '0.7.1'
usbredir_version_info = '>= @0@'.format(usbredir_version)
d1 = dependency('libusbredirparser-0.5', version: usbredir_version_info, required : get_option('usbredir'))
d2 = dependency('libusbredirhost', version: usbredir_version_info, required : get_option('usbredir'))
d3 = dependency('libusb-1.0', version : '>= 1.0.21', required : get_option('usbredir'))
summary_info += {'usbredir': d1.found() and d2.found() and d3.found()}
if d1.found() and d2.found() and d3.found()
  if target_machine.endian() == 'little'
    spice_glib_deps += [d1, d2, d3]
    spice_gtk_config_data.set('USE_USBREDIR', '1')
    spice_gtk_has_usbredir = true
  else
    warning('USB redirection disabled on big endian machine as ' +
            'usbredir only support little endian')
  endif
endif

d = dependency('libcap-ng', required : get_option('libcap-ng'))
summary_info += {'libcap-ng': d.found()}
if d.found()
  spice_gtk_config_data.set('USE_LIBCAP_NG', '1')
  spice_acl_deps += d
endif

# polkit
spice_gtk_has_polkit = false
d = dependency('polkit-gobject-1', version : '>= 0.101', required : get_option('polkit'))
summary_info += {'polkit': d.found()}
if d.found()
  spice_gtk_policy_dir = d.get_pkgconfig_variable('policydir')

  # TODO:  With 'auto', we should just disable polkit support if this is missing.
  if not compiler.has_function('acl_get_file')
    acl_dep = compiler.find_library('acl')
    if not compiler.has_function('acl_get_file', dependencies : acl_dep)
      error('PolicyKit support requested, but some required packages are not available')
    endif
    spice_acl_deps += acl_dep
  endif

  spice_acl_deps += d
  spice_acl_deps += dependency('gio-unix-2.0')
  spice_gtk_config_data.set('USE_POLKIT', '1')
  spice_gtk_has_polkit = true
endif

if spice_gtk_has_usbredir and not spice_gtk_has_polkit
  warning('Building with usbredir support, but *not* building the usb acl helper')
endif

# pie
spice_gtk_has_pie = false
if get_option('pie')
  spice_gtk_has_pie = true
endif
summary_info += {'pie': spice_gtk_has_pie}

# usb-acl-helper-dir
spice_gtk_usb_acl_helper_dir = get_option('usb-acl-helper-dir')
if spice_gtk_usb_acl_helper_dir.strip() == ''
  spice_gtk_usb_acl_helper_dir = spice_gtk_prefix / get_option('libexecdir')
endif
spice_gtk_config_data.set_quoted('ACL_HELPER_PATH', spice_gtk_usb_acl_helper_dir)
summary_info += {'usb-acl-helper-dir': spice_gtk_usb_acl_helper_dir}

# usb-ids-path
spice_gtk_usb_ids_path = get_option('usb-ids-path')
if spice_gtk_usb_ids_path.strip() == ''
  usbutils = dependency('usbutils', required : false)
  if usbutils.found()
    spice_gtk_usb_ids_path = usbutils.get_pkgconfig_variable('usbids')
  endif
endif
summary_info += {'usb-ids-path': spice_gtk_usb_ids_path}

if spice_gtk_usb_ids_path.strip() != ''
  spice_gtk_config_data.set('WITH_USBIDS', '1')
  spice_gtk_config_data.set_quoted('USB_IDS', spice_gtk_usb_ids_path)
endif

# coroutine
spice_gtk_coroutine = get_option('coroutine')
if spice_gtk_coroutine == 'auto'
  if host_machine.system() == 'windows'
    spice_gtk_coroutine = 'winfiber'
  else
    spice_gtk_coroutine = 'ucontext'
  endif
endif

if spice_gtk_coroutine == 'ucontext'
  foreach f : ['makecontext', 'swapcontext', 'getcontext']
    if not compiler.has_function(f)
      error('Function missing:' + f)
    endif
  endforeach
  spice_gtk_config_data.set('WITH_UCONTEXT', '1')
  if host_machine.system() == 'darwin'
    spice_gtk_config_data.set('_XOPEN_SOURCE', '1')
  endif
  endif

if spice_gtk_coroutine == 'libucontext'
  d = dependency('libucontext')
  spice_glib_deps += d
  spice_gtk_config_data.set('WITH_UCONTEXT', '1')
  spice_gtk_config_data.set('HAVE_LIBUCONTEXT', '1')
endif

if spice_gtk_coroutine == 'gthread'
  spice_gtk_config_data.set('WITH_GTHREAD', '1')
endif

if spice_gtk_coroutine == 'winfiber'
  spice_gtk_config_data.set('WITH_WINFIBER', '1')
endif
summary_info += {'coroutine': spice_gtk_coroutine}

# introspection
spice_gtk_has_introspection = false
d = dependency('gobject-introspection-1.0', version : '>= 0.94', required : get_option('introspection'))
if d.found()
  spice_glib_deps += d
  spice_gtk_has_introspection = true
endif
summary_info += {'introspection': spice_gtk_has_introspection}

# vala (depends on introspection)
spice_gtk_has_vala = false
d = dependency('vapigen', required : get_option('vapi'))
if d.found()
  if not spice_gtk_has_introspection
    error('VAPI support requested without introspection')
  endif
  spice_gtk_has_vala = true
endif
summary_info += {'vapi': spice_gtk_has_vala}

# lz4
d = dependency('liblz4', required : get_option('lz4'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('USE_LZ4', '1')
endif
summary_info += {'lz4': d.found()}

# sasl
d = dependency('libsasl2', required : get_option('sasl'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('HAVE_SASL', '1')
endif
summary_info += {'sasl': d.found()}

# smartcard check
d = dependency('libcacard', version : '>= 2.5.1', required : get_option('smartcard'))
if d.found()
  spice_glib_deps += d
  spice_gtk_config_data.set('USE_SMARTCARD', '1')
endif
summary_info += {'smartcard': d.found()}

# valgrind
if get_option('valgrind')
  if spice_gtk_config_data.get('HAVE_VALGRIND_VALGRIND_H', '0') != '1'
    error('Valgrind requested but headers not found')
  endif
  spice_gtk_config_data.set('HAVE_VALGRIND', '1')
endif
summary_info += {'valgrind': get_option('valgrind')}
#
# global C defines
#
glib_encoded_version = 'GLIB_VERSION_@0@'.format(glib_version.underscorify())
spice_gtk_global_cflags += ['-DGLIB_VERSION_MIN_REQUIRED=@0@'.format(glib_encoded_version),
                            '-DGLIB_VERSION_MAX_ALLOWED=@0@'.format(glib_encoded_version)]

if spice_gtk_has_gtk
  gtk_encoded_version='GDK_VERSION_@0@'.format(gtk_version_required.underscorify())
  spice_gtk_global_cflags += ['-DGDK_VERSION_MIN_REQUIRED=@0@'.format(gtk_encoded_version),
                              '-DGDK_VERSION_MAX_ALLOWED=@0@'.format(gtk_encoded_version)]
endif

# Workaround gtk+ exposing Objective C: https://gitlab.gnome.org/GNOME/gtk/issues/1737
if host_machine.system() == 'darwin'
  spice_gtk_global_cflags += ['-ObjC']
endif

add_project_arguments(compiler.get_supported_arguments(spice_gtk_global_cflags),
                      language : 'c')

#
# write config.h
#
proj_version = meson.project_version()
proj_name = meson.project_name()
config_data = {'VERSION' : proj_version,
               'PACKAGE_VERSION' : proj_version,
               'GETTEXT_PACKAGE' : proj_name,
               'LOCALE_DIR' : spice_gtk_localedir,
               'PACKAGE_STRING' : '@0@ @1@'.format(proj_name, proj_version),
               'PACKAGE_BUGREPORT' : 'spice-devel@lists.freedesktop.org'}
foreach key, value : config_data
  spice_gtk_config_data.set_quoted(key, value)
endforeach

configure_file(output : 'config.h',
               configuration : spice_gtk_config_data)

#
# Subdirectories
#
subdir('src')
subdir('tools')
subdir('tests')
if build_machine.system() == 'windows'
  message('Disabling gtk-doc while building on Windows')
else
  d = find_program('gtkdoc-scan', required : get_option('gtk_doc'))
  if d.found()
    subdir('doc')
  endif
  summary_info += {'gtk_doc': d.found()}
endif
subdir('data')
subdir('man')
subdir('po')
subdir('vapi')

#
# write spice-client-glib.pc
#
pkgconfig = import('pkgconfig')
pkgconfig.generate(spice_client_glib_lib,
                   description : 'SPICE Client GLib 2.0 library',
                   subdirs : 'spice-client-glib-2.0',
                   requires : ['spice-protocol', 'glib-2.0', 'gobject-2.0', 'gio-2.0'],
                   variables : 'exec_prefix=${prefix}')

#
# write spice-client-gtk.pc
#
if spice_gtk_has_gtk
  pkgconfig.generate(spice_client_gtk_lib,
                     description : 'SPICE Client Gtk 3.0 library',
                     subdirs : 'spice-client-gtk-3.0',
                     requires : ['spice-client-glib-2.0', 'gtk+-3.0', 'glib-2.0', 'gobject-2.0'],
                     variables : 'exec_prefix=${prefix}')
endif

summary(summary_info, bool_yn: true)
