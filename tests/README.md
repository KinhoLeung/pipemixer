# Board integration tests

The localization text/locale test also runs on a native host with UTF-8 locales
and ncursesw. It checks malformed UTF-8 without mutating an existing string,
translated parameter IDs and positional formats, and actual curses cells at
the right edge of a clipped Chinese label:

```sh
cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
  $(pkg-config --cflags ncursesw) tests/i18n_test.c src/i18n.c src/tui/text.c \
  src/collections/wstring.c src/xmalloc.c -o /tmp/pipemixer-i18n-test \
  $(pkg-config --libs ncursesw)
ASAN_OPTIONS=detect_leaks=0 /tmp/pipemixer-i18n-test
```

Run against an existing PipeWire session with WirePlumber, Python 3, `pw-cli`,
`pw-link` and `pw-dump`. The PCM suites also need `audio-probe` and the loopback,
filter-chain and builtin filter plugins. Tests use isolated virtual nodes and
synthetic stereo tones; they leave the default device unchanged and remove
their own paths in `finally` blocks. Run the suites sequentially. Test group
names beginning with `test_` and `pipemixer.board-test.` are reserved while the
suite runs; those names must not already be in use.

Build pipemixer with `./build.sh` for the RK3506 board. Build the probe with the
same cross compiler and sysroot (adjust `SDK_HOST` for your SDK):

```sh
SDK_HOST=/home/kinho/Lyra-sdk/buildroot/output/rockchip_rk3506_luckfox/host
SDK_SYSROOT="$SDK_HOST/arm-buildroot-linux-gnueabihf/sysroot"
"$SDK_HOST/bin/arm-buildroot-linux-gnueabihf-gcc" -O2 -Wall -Wextra -Wno-unused-parameter \
  -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
  -I"$SDK_SYSROOT/usr/include/pipewire-0.3" -I"$SDK_SYSROOT/usr/include/spa-0.2" \
  tests/audio_probe.c -o .cache/audio-probe -lpipewire-0.3 -lm
```

Upload the binary, `pipemixer-dynamics.so`, probe and all `board_*.py` files into `/tmp/board` on
the board. Mark both binaries executable, then run on the board:

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PIPEMIXER_BINARY=/tmp/board/pipemixer
export PIPEMIXER_LANGUAGE=en
python3 /tmp/board/board_i18n.py
python3 /tmp/board/board_graph.py
python3 /tmp/board/board_routing.py
python3 /tmp/board/board_routing_groups.py
python3 /tmp/board/board_routing_batch.py
python3 /tmp/board/board_audio.py
python3 /tmp/board/board_effects.py
python3 /tmp/board/board_effect_chain.py
python3 /tmp/board/board_effect_chain_failures.py
python3 /tmp/board/board_compressor.py
python3 /tmp/board/board_limiter.py
python3 /tmp/board/board_diagnostics.py
python3 /tmp/board/board_history.py
python3 /tmp/board/board_recording.py
python3 /tmp/board/board_monitoring_resources.py
python3 /tmp/board/board_capture_control.py
python3 /tmp/board/board_scenes.py
python3 /tmp/board/board_recovery.py
python3 /tmp/board/board_profiles.py
python3 /tmp/board/board_rules.py
python3 /tmp/board/board_rule_ui.py
python3 /tmp/board/board_rule_batch.py
python3 /tmp/board/board_rule_priority.py
python3 /tmp/board/board_rule_fallback.py
python3 /tmp/board/board_monitors.py
python3 /tmp/board/board_monitor_ui.py
```

- Graph: compare ports/links with `pw-dump`; observe external links and removal.
- Localization: isolated Chinese/English config and locale selection, F2 across
  panels and during a fade, Chinese search and mouse tab geometry, narrow CJK
  menu borders, real effect parameter edits and monitor/Solo controls, WAV
  finalization with Unicode paths, byte-locale fallback and unchanged JSON.
  The suite selects its own language and restores its inherited environment.
- Routing: native links, fan-out, idempotency, feedback rejection, keyboard and
  mouse matrix control, terminal resizing and endpoint removal/reappearance.
- Routing groups: node/type headers, independently folded axes, class filters,
  UTF-8 search/cancel/backspace, device reappearance and preservation of links.
  `board_terminal.py` observes the rendered ncurses display using standard Python.
- Manual batches: node/group multi-selection across sources and destinations,
  channel matching, idempotency, selected-only disconnect, a cycle formed only
  by the combined plan, stale/recycled port IDs, real incompatible-format
  errors and cancellation rolling back only new links. The digital-format probe
  uses a fresh source channel to prevent PipeWire reusing an existing format.
- Monitors: simultaneous main/monitor PCM proves mix and multi-Solo isolation,
  gain/mute independence, listening source changes and restoration after Solo,
  missing source silence/reappearance, engine restart, scene ownership,
  malformed reload retention and foreign/unmanaged input conflicts. TUI covers
  M/S/L/U, mix/Solo lists, source choice, resize, matrix/menu input separation,
  disable/enable/delete and continued audio after TUI exit.
- Audio: concurrent bus creation, actual mixing of 440/880 Hz tones,
  independent send gain/mute, internal feedback detection and TUI persistence.
  The TUI test waits for asynchronous creation to finish before closing the UI.
- Effects: actual 6 dB EQ gain, idle/live parameters without replacing nodes,
  bypass, high-pass filtering, noise gating, custom graphs and invalid graph
  cleanup; TUI creation, adjustment, reset, bypass, resize and removal.
- Editable chains: empty chain, adding/removing/reordering processors, DSP
  order measured by PCM, live per-stage bypass, preservation of manual stereo
  links and Send gains, loading different chains over existing ones, retention
  of manual edits with recovery active, TUI editing and signal exit during an
  edit. A bad graph retains original nodes; terminating a replacement worker
  restores the original graph, parameters, links and Send gain.
- Dynamics: compressor ratio/threshold, makeup and parallel mix, limiter sample
  ceilings under input boost, combined chains and scene/startup reconstruction;
  TUI adjustment/reset/bypass and actual float PCM. Detectors are per channel;
  the limiter is a sample peak limiter without oversampling/lookahead.

- Diagnostics: analytic stereo peak/RMS, overload counters, real PipeWire
  profiler timing/rate/quantum, CPU/RSS, unavailable sources and TUI counter
  reset/resize/menu isolation. Diagnostics use the client profiler protocol.
- History: separate 440/880 Hz stereo float WAVs with equal frame ranges,
  repeated ring wraps over 60 seconds, RSS plateau/CPU/drop counters, malformed
  requests and duplicate/memory-budget rejection, missing-source silence and
  stable-name reconnect. Internal captures are hidden from graph snapshots.
- Recording: continuous 60-second two-track runs with preroll and default
  segmentation, eight-track resource measurements, valid RIFF/fact/data lengths
  and tone content, missing sources/reconnect, SIGTERM finalization, configurable
  segment continuity, export/recording exclusion, existing-directory and
  admission/runtime disk-reserve protection. TUI covers
  source selection, async export/recording, resize and continued recording after
  exit. Files use /tmp for resource tests; tmpfs costs consume RAM and do not
  represent SD/eMMC/USB write performance. `board_monitoring_resources.py` measures
  process RSS during sustained diagnostics/history-menu refresh.
  `board_capture_control.py` additionally verifies concurrent same-name creation,
  text/JSON control, actual TUI two-source selection and stop-cache finalization.
  Each fixture deletes only its own sessions/files and retains pre-existing user nodes and link IDs.

- Scenes: self-contained graph snapshots, channel levels, defaults, DSP/bypass,
  recreation with new IDs and without the original custom config, scene
  switching and actual PCM restoration; conflict/missing-device rejection,
  malformed/cyclic scenes, atomic concurrent writes, offline listing and TUI
  save/load/delete. Uses a separate config directory under `/tmp/board` and
  restores the previous defaults after testing.
- Selective recovery: late/returning endpoint channel gains, mute, defaults,
  skipped send creation, repeated source/send generation changes, preserved
  live bus/EQ/send values, engine restart ledger, application stream target
  recovery and explicit disable; actual PCM before/after reconnection.
- Hardware recovery (Luckfox): saves stable ALSA Profile/Route names, restores
  the USB gadget card from off, re-enumerates real cards by restarting
  WirePlumber, verifies gains/mute/sends/Solo and independent PCM. Restores
  original hardware settings. The gadget clock requires its USB host, so
  its links are checked separately from the virtual PCM measurements.
  Tests unknown names, blocked route reporting and v1 scene compatibility.
- Automatic routing: offline rule creation/validation, one engine per config
  directory, stereo links after endpoint reappearance with new IDs, no fallback,
  source gain retained, feedback blocking, ownership and independent rule sets,
  malformed reload retaining the last good rules, and scene-lock coordination.
  The TUI suite saves rules from the matrix, toggles/deletes rules, resizes the
  menu, and checks reconnection with the TUI closed. Both use isolated configs.
- Batch: version 1/2 compatibility, channel-safe glob expansion, multiple
  source/target nodes, dynamic new ports, manual connection retention, rejection
  of implicit mono/stereo mapping and TUI A creation with the TUI closed.
- Priority: independent election per source node, late higher-priority targets,
  lower-rule takeover, lexical ties, offline edits, ungrouped fan-out and TUI
  priority adjustment without changing source levels.
- Fallback: ordered stereo candidate selection, delayed failback, primary
  flapping, preserved gain/mute and actual PCM; scene ownership restoration,
  two real incompatible-format candidates, daemon survival and strict offline
  JSON validation. `audio-probe reject NODE_NAME` supplies stereo S16-only
  filter ports that cannot negotiate with DSP F32 source ports.

Startup restoration tests span a real reboot and use persistent fixture
files. Install `scripts/install-board-startup` on the board as described in
the main README, then copy all `board_*.py` helpers and the probe to
`/root/.local/share/pipemixer-tests/`. Run these commands on the board:

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PIPEMIXER_BINARY=/usr/local/bin/pipemixer
python3 /root/.local/share/pipemixer-tests/board_startup.py prepare
/etc/init.d/S98pipemixer restart
python3 /root/.local/share/pipemixer-tests/board_startup.py verify
sync
reboot
# Reconnect over SSH and export the two variables above again.
python3 /root/.local/share/pipemixer-tests/board_startup.py verify --reboot
python3 /root/.local/share/pipemixer-tests/board_startup.py cleanup
```

The fixture checks offline startup selection, selection/disable in the TUI,
missing-device/stream pruning, and actual PCM through restored buses, sends,
EQ and a self-contained custom effect. It checks channel levels and parameters
after restarting the service and after a changed kernel boot ID. Service
and board restart checks also compare saved hardware Profile/Route and levels,
then recreate a late external endpoint and verify its per-channel levels,
mute and skipped send are replayed automatically without changing the main PCM.
Service startup is idempotent with one PipeWire and one WirePlumber process. Cleanup
restores the previous startup selection and audio graph. This suite uses the
real root config directory; do not leave its prepared fixture enabled after
testing. Keep its state file until cleanup completes.

For startup integration with automatic routing, use `board_rule_boot.py`
instead of `board_startup.py` in the prepare/verify/cleanup commands above.
Copy it and `board_rules.py` to the persistent helper directory. This adds
persisted rules to the startup fixture, verifies the supervised engine after
audio-session restart and board reboot, repairs a deliberately lost link,
and connects an endpoint that appears later. It checks the existing channel
levels, EQ and actual PCM before and after late connection. Cleanup removes
only the fixture's rules and restores the original scene selection and graph.

Use `board_rule_advanced_boot.py` in the same prepare/verify/cleanup sequence
for version 2 startup integration. It persists batch rules, priorities, an
exclusive group, a backup list and failback delay; two late sources switch
between primary, backup and the lower-priority rule after service restart and
after a changed boot ID. The original startup chain retains its saved levels,
EQ and measured PCM. All fixture rules and nodes are removed during cleanup.

Use `board_monitor_boot.py` in that sequence to include independent monitoring
and all advanced routing/startup checks. It persists normal mix sources,
listening choice and active Solo, reconnects late headphones, recreates a
removed monitor worker and compares main/Solo/restored-listen PCM after service
restart and a real board reboot. The wrapper deletes its monitor before the
original graph/scene fixture is restored. Keep fixtures until cleanup completes.

The event-queue regression can run on the host without PipeWire. It forces
callbacks to grow the queue during dispatch and verifies the after callback
and all nested events. Address/undefined sanitizers verify memory lifetime:

```sh
cc -g -fsanitize=address,undefined -Isrc -Wno-unused-parameter \
  tests/events_reentrant.c src/events.c src/xmalloc.c -o /tmp/pipemixer-events-test
ASAN_OPTIONS=detect_leaks=0 /tmp/pipemixer-events-test
```

Captured PCM is stereo 48 kHz float. Measurements use RMS after discarding
the first 0.5 seconds and reject nonfinite samples. The probe allocates before
processing and writes files outside its realtime callback. The volume CLI's
percentage uses the existing cubic scale: 50% corresponds to gain 0.125.
Test sends explicitly reset gain/mute because WirePlumber can remember them
across repeated runs. The TUI diagnostic log is `/tmp/board/routing-test.log`.
The startup monitor test records an Audio/Sink through `audio-probe capture-sink`,
which explicitly selects its monitor ports; ordinary `capture` targets sources.

The PCM probe gives playback streams distinct media names so unrelated tests
do not share remembered application gains. Captures disable property restoration
and all probe streams disable remembered destinations. Its
`play-reconnect` mode permits explicit stream destination changes for the
application recovery tests; ordinary playback retains no-reconnect behavior.

The bundled DSP can also be checked independently of PipeWire on the host:

```sh
gcc -O2 -shared -fPIC src/dsp/dynamics.c -o .cache/native-dynamics.so -lm
gcc -O2 -Isrc tests/dynamics_test.c -o .cache/native-dynamics-test -ldl -lm
.cache/native-dynamics-test "$PWD/.cache/native-dynamics.so"
```

This checks the compressor against its analytic transfer curve, soft knee,
Attack/Release and mixing, and checks the limiter with impulses, sustained
overload, release and non-finite inputs. Build the same test for ARM with the
SDK compiler, copy it beside the board plugin and run
`./dynamics-test ./pipemixer-dynamics.so`.

After installation, copy helpers/probe into `/root/.local/share/pipemixer-tests`.
The dynamics startup fixture requires the board's startup selection to be off
and no active recovery scene; it saves the current hardware settings, creates
an isolated three-processor chain and Send, then selects its scene for startup:

```sh
export PIPEMIXER_BINARY=/usr/local/bin/pipemixer
python3 /root/.local/share/pipemixer-tests/board_dynamics_boot.py prepare
/etc/init.d/S98pipemixer restart
python3 /root/.local/share/pipemixer-tests/board_dynamics_boot.py verify
# Reboot the board and reconnect before the following two commands.
python3 /root/.local/share/pipemixer-tests/board_dynamics_boot.py verify --reboot
python3 /root/.local/share/pipemixer-tests/board_dynamics_boot.py cleanup
```

Cleanup restores the saved hardware state, removes the fixture and switches
startup restoration off. Its records and probe survive the board reboot.


Stage 6 automation tests run against the candidate or installed binary:

```sh
python3 /tmp/board/board_automation_fades.py
python3 /tmp/board/board_automation_rules.py
python3 /tmp/board/board_automation_integration.py
python3 /tmp/board/board_external_control.py
python3 /tmp/board/board_automation_ui.py
python3 /tmp/board/board_automation_supervisor.py
python3 /tmp/board/board_automation_resources.py
```

Copy `scripts/pipemixer-automation-session` into `/tmp/board/` for the supervisor
test. Each test uses a separate XDG_CONFIG_HOME, cleans its own nodes/definitions,
and checks the original graph. The MIDI test uses a raw PTY character device;
physical MIDI hardware is not covered. The resource test measures engine memory
and engine/PipeWire/WirePlumber CPU during eight concurrent fades. It requires
WirePlumber to stay below 85% of one core, checks all eight controls advance, 24
fresh quiet volume queries and eight concurrent starters, and
verifies an unrelated short sequence finishes during a long fade/wait. Policy
integration covers route-rule and monitor actions plus link/default/parameter
conditions. External controls include OSC scene/parameter/rule methods and rapid
MIDI press/release bursts with verified final mute and parameter states.
See [automation usage](../docs/AUTOMATION.md) for the persistent start/stop preference.

For the installed service's real reboot test, copy `board_automation_boot.py` and
`board_graph.py` to a persistent directory first. The fixture requires startup
scene restoration to be off, no active recovery scene and no existing root
automation definitions. It saves the current audio state and engine preference,
installs rules targeting only its own late virtual bus, then restores those
settings during cleanup:

```sh
export XDG_RUNTIME_DIR=/run/user/0
export PIPEMIXER_BINARY=/usr/local/bin/pipemixer
python3 /root/.local/share/pipemixer-tests/stage6-helpers/board_automation_boot.py prepare
# Reboot the board and reconnect, then export the same two variables.
python3 /root/.local/share/pipemixer-tests/stage6-helpers/board_automation_boot.py verify
python3 /root/.local/share/pipemixer-tests/stage6-helpers/board_automation_boot.py cleanup
```

Verification checks a changed kernel boot ID, the persisted definitions/OSC
mapping, a late target's fade, installed supervisor SIGKILL recovery, reset
counters and persistent off after a supervisor restart. Always run cleanup,
including after a failed verification; the state file survives interruptions.

`board_metadata_unbind.py` tests the board's PipeWire metadata compatibility fix.
It briefly pauses the sole WirePlumber exporter, binds and kills eight metadata
clients before their snapshot handshake completes, then resumes WirePlumber.
With the original module, `--expect-bug` requires a lost live notification and a
confirmed switch visible through a fresh metadata query. With the patched module,
the default switch and the preexisting metadata watcher must both update. It
always resumes the exporter and restores its own bus/default fixture; run it in
the isolated board test session rather than during a live performance.
