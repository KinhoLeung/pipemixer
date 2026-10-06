project('pipemixer', 'c',
  version: '0.5.1',
  license: 'GPL-3.0-or-later',
  default_options: ['warning_level=3'])

add_project_arguments('-Wno-pedantic', language: 'c')
add_project_arguments('-Wno-unused-value', language: 'c')
add_project_arguments('-Wno-unused-parameter', language: 'c')

git = find_program('git', required: false)
if git.found()
  git_tag = run_command(git, 'describe', '--tags', check: false).stdout().strip()
  if git_tag != ''
    add_project_arguments('-DPIPEMIXER_GIT_TAG="@0@"'.format(git_tag), language: 'c')
  endif
  git_branch = run_command(git, 'branch', '--show-current', check: false).stdout().strip()
  if git_branch != ''
    add_project_arguments('-DPIPEMIXER_GIT_BRANCH="@0@"'.format(git_branch), language: 'c')
  endif
endif
add_project_arguments('-DPIPEMIXER_VERSION="@0@"'.format(meson.project_version()), language: 'c')

pipewire_dep = dependency('libpipewire-0.3')
ncursesw_dep = dependency('ncursesw')
inih_dep = dependency('inih')

cc = meson.get_compiler('c')
m_dep = cc.find_library('m')
add_project_arguments('-DPIPEMIXER_DSP_DIR="' + join_paths(get_option('prefix'), get_option('libdir'), 'pipemixer') + '"', language: 'c')
shared_library('pipemixer-dynamics', 'src/dsp/dynamics.c', name_prefix: '', dependencies: m_dep,
  install: true, install_dir: join_paths(get_option('libdir'), 'pipemixer'))

# would preferably use #embed for this but it's a C23 feature
default_config_source = custom_target(
  'default_config.c',
  input: 'assets/pipemixer.ini',
  output: 'default_config.c',
  feed: true,
  capture: true,
  command: [ find_program('awk'), '-f', files('assets/mkconfig.awk') ]
)

effect_config_sources = []
foreach preset : ['eq', 'voice']
  effect_config_sources += custom_target(
    preset + '_config.c',
    input: 'assets/effects/' + preset + '.conf',
    output: preset + '_config.c',
    feed: true,
    capture: true,
    command: [find_program('awk'), '-v', 'symbol=' + preset + '_config', '-f', files('assets/mkconfig.awk')],
  )
endforeach

pipemixer_sources = [
  'src/pipemixer.c',
  'src/cli.c',
  'src/cli-graph.c',
  'src/scene.c',
  'src/route-rules.c',
  'src/monitor.c',
  'src/diagnostics.c',
  'src/recorder.c',
  'src/automation.c',
  'src/automation-config.c',
  'src/external-control.c',
  'src/tui/diagnostics-ui.c',
  'src/tui/recording-ui.c',
  'src/tui/automation-ui.c',
  'src/log.c',
  'src/xmalloc.c',
  'src/utils.c',
  'src/config.c',
  'src/events.c',
  'src/format.c',
  'src/tui/tui.c',
  'src/tui/routing.c',
  'src/tui/routing-view.c',
  'src/tui/routing-batch.c',
  'src/tui/monitors.c',
  'src/tui/effects.c',
  'src/tui/pad.c',
  'src/tui/menu.c',
  'src/collections/vec.c',
  'src/collections/map.c',
  'src/collections/string.c',
  'src/collections/wstring.c',
  'src/collections/dict.c',
  'src/pw/common.c',
  'src/pw/graph.c',
  'src/pw/managed.c',
  'src/pw/effects.c',
  'src/pw/effect-chain.c',
  'src/pw/types.c',
  'src/pw/device.c',
  'src/pw/node.c',
  'src/pw/peak.c',
  default_config_source,
  effect_config_sources
]

include_dirs = [
  'src'
]

executable('pipemixer', pipemixer_sources,
  include_directories: include_dirs,
  dependencies: [ncursesw_dep, pipewire_dep, m_dep, inih_dep, dependency('threads')],
  install: true)

install_data(
  'assets/io.github.heather7283.pipemixer.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'),
)

install_data('assets/automation/example.json',
  install_dir: join_paths(get_option('datadir'), 'pipemixer', 'automation'))

conf_data = configuration_data()
conf_data.set('VERSION', meson.project_version())

foreach man: [ 'pipemixer.1', 'pipemixer.ini.5' ]
  configure_file(
    input: join_paths('man', '@0@.in'.format(man)),
    configuration: conf_data,
    output: man,
    install: true,
    install_dir: join_paths(get_option('mandir'), 'man@0@'.format(man.split('.')[-1])),
  )
endforeach

if get_option('shell-completions')
  zsh_install_dir = join_paths(get_option('datadir'), 'zsh', 'site-functions')
  install_data('completions/_pipemixer', install_dir: zsh_install_dir)
endif
