# pipemixer

[中文总体说明](README.zh-CN.md)

This is a TUI volume control application for [pipewire] built with [ncurses].
Heavily inspired by [pulsemixer] and [pwvucontrol].

![Screenshot](assets/screenshot.png)

The channel rows show a live peak meter in dBFS beside the volume setting.
Meters monitor only the selected tab and decay after playback stops.

The TUI supports English and Simplified Chinese, including the routing matrix,
scenes, monitors, effects, diagnostics, recording and automation panels. Press
`F2` to switch languages for the current session. This closes an open menu;
matrix filters/folds and running audio, fades and recordings continue.

Use `PIPEMIXER_LANGUAGE=zh_CN pipemixer` for a Chinese session or
`PIPEMIXER_LANGUAGE=en pipemixer` for English. To persist your preference, set
`language=zh_CN` or `language=en` in the existing `[main]` section of
`~/.config/pipemixer/pipemixer.ini`. The default `language=auto` follows
`LC_ALL`, `LC_MESSAGES`, then `LANG`; Chinese locales select Simplified Chinese.
The environment override takes precedence over the INI. Chinese requires a
UTF-8 locale and terminal font; an explicit `LC_ALL=C` keeps English active.
CLI commands, JSON keys, stable names and parameter identifiers keep their
original values. Rebind the switch with `toggle-language=f2` under `[binds]`;
function key names `f1` through `f12` are supported.

On a playback or recording stream, press `c` to choose an output or input device.
Choose `Follow default` to let PipeWire use the system default again.
The device port menu remains on `p`.

Press `r` to open the live audio routing matrix. Rows are output ports; columns
are input port IDs. Move with arrows or `hjkl`, and use Enter/Space or click a
cell to connect or disconnect that pair. The selected endpoints are shown below
the matrix. `+` means connected, `.` disconnected, `~` negotiating, and `!` an
error. Press `r` or Escape to return. The matrix scrolls in both directions and
updates when nodes or links appear/disappear. New connections that would form a
feedback loop are rejected. Connections persist after pipemixer exits, until
removed or an endpoint disappears; WirePlumber may also change connections.

Press `v` to cycle flat ports, node groups and type groups. `z` folds the selected
output group and `Z` folds the input group. Enter/Space on a group header folds
it. Press `f` to filter devices, buses (including monitors), sends, effects or
applications. `/` edits a case-insensitive search, Enter applies it, Escape
restores the previous search, and Ctrl-U clears it. Folding and filtering only
change the view; existing audio connections keep running.

Mark output ports/groups with `x`, input ports/groups with `X`, or all channels
of the current node with `n`/`N`. Marked ports have `*`; `O:`/`I:` show counts.
`c` connects all selected matching channels and `d` disconnects those pairs.
`u` clears selection. Known channels match by name (FL to FL, FR to FR); ports
without channel names only match a single unnamed output/input pair. The whole
connection plan is checked for feedback before starting. A failed or cancelled
connection batch removes its own new links and keeps pre-existing links;
Escape cancels an active batch. A cancelled disconnect can leave a partial
result. Enabled automatic rules can recreate disconnected links. Folding keeps
marks; applying a different search or type filter clears them. Missing/replaced
ports lose their marks. Shift-click a row/column header to mark it; `?` shows
the matrix controls.

Press `M` to create/select an independent monitor, edit its normal mix and Solo
sources, choose a listening source, or enable/delete it. From the matrix,
select a source output and headphone/monitor destination first; creation uses
that source and preselects the destination. Groups belonging to one node also
work. The monitor gets its own audio path, volume and mute; it changes neither
the source levels nor the main output/recording connections. On the selected
monitor, `S` toggles Solo for the focused source (matrix output or mixer node),
`L` listens to that source, and `U` clears all Solo. Multiple Solo sources mix
together. Clearing Solo restores the previous listening source or normal mix.
Changes to the listening source while Solo is active take effect after clearing
Solo. In source lists, Enter toggles membership, `g/G` selects first/last, and
Escape closes the list. The matrix and monitor menu show live mode/status.

In the routing matrix, press `a` to save the selected output/input pair as an
automatic routing rule (`route1`, `route2`, ...). From the mixer, `a` opens
the rule menu with live connection states, enable/disable and delete actions.
Enabled rules reconnect when either endpoint reappears under the same stable
name, including after object IDs change. Configure both channels for stereo.
Press `A` in the matrix to save all compatible channels between the selected
two nodes as one batch rule. In the rule menu, `h/l` or Left/Right changes the
selected rule's priority by 10. Counts, priority, group and fallback status
are shown in the menu.

Press `b` to create a stereo virtual bus, create an independent send from the
focused audio node, or delete a managed bus/send/effect. A bus has a virtual
output device for mixing applications and a virtual microphone carrying the
mix. Each send copies audio to another device or bus and has its own output
volume and mute controls. These paths use PipeWire's loopback module.

Press `e` to create an equalizer, a voice preset or an empty chain. On an
effect's input/output device, `e` opens its ordered processor list. Use `j/k`
to select and Enter to edit that processor's parameters. Press `a` to add,
`d` to remove (with a confirmation menu), `K/J` to move up/down, Space to
bypass one processor, and `B` to bypass the whole chain. In the parameter
page, `h/l` or Left/Right adjust, Enter resets, and Esc returns to the chain.
Chains support up to 16 processors, including shelf/parametric EQ, high/low
pass filters, gain, a noise gate, a compressor and a peak limiter.
Custom graphs keep their parameter editor.
Adding, removing or moving a processor briefly interrupts that chain while its
nodes are rebuilt; current connections, channel gains, mute and dependent Sends
are restored. Parameter edits and bypass on edited chains use live controls.
The first per-processor bypass on an unedited preset also rebuilds it as an
editable chain.
Scenes retain processor order, parameters and individual bypass states.
Effects use PipeWire's filter-chain module and can
also be loaded from a custom configuration through the CLI. Route audio into
the effect's virtual output device and take the processed signal from its
virtual microphone; use sends or the routing matrix to join chains.

Press `s` to save the current setup as a new scene, load a saved scene, delete
one, or select the scene to restore on startup. The menu header shows the
startup selection, and its last item disables restoration. TUI saves use the
first available name (`scene1`, `scene2`, ...); the CLI
accepts a name of your choice. Scenes include buses, sends, effect definitions
(including custom graphs), DSP parameters and bypass, per-channel volumes,
mute, stream destinations, defaults, and audio connections. They use exact
node/port names instead of session IDs.

Wait for the creation notice to confirm success. Completed paths run in
detached audio workers and survive closing the TUI or CLI. They last for the
current PipeWire session. The optional board startup
service recreates paths from the selected scene after PipeWire restarts.
Delete paths through `b` or the matching CLI command. Worker errors are
logged to `$XDG_RUNTIME_DIR/pipemixer-KIND-NAME.log`. Buses and effects have no
automatic physical-device connections. Creating sends and effects requires a running
session manager such as WirePlumber.

Mouse controls are enabled by default. Click a tab to switch views or a row to
focus it. Click a stream title to choose its device, a device port or card
profile row to open its menu, the volume bar to set volume, or the volume
number to toggle mute. Click a menu choice to apply it; click outside to cancel.
The wheel scrolls lists or menu choices. Right-click a stream to choose its
device or an output/input device to make it the default. Middle-click a node
to toggle mute. Set `mouse=false` under `[main]` to disable mouse capture.

## Building
```
git clone https://github.com/heather7283/pipemixer
cd pipemixer
meson setup build
meson compile -C build
```

## Running
```
pipemixer -h
```
To debug:
```
pipemixer -l trace -L 4 4>pipemixer.log
```
With valgrind:
```
valgrind --leak-check=full --show-leak-kinds=all --track-fds=yes --log-fd=5 -- pipemixer -l trace -L 4 4>pipemixer.log 5>valgrind.log
```

## Noninteractive control
Run a command to query or change PipeWire state without opening the TUI. Put options before the command:

```sh
pipemixer list nodes
pipemixer list devices
pipemixer list ports
pipemixer list links
pipemixer --json list graph
pipemixer connect 'my-source:monitor_FL' 'my-sink:playback_FL'
pipemixer disconnect id:52 id:64
pipemixer create-route-rule speakers_left 'my-source:capture_FL' 'my-sink:playback_FL'
pipemixer create-route-rule speakers_right 'my-source:capture_FR' 'my-sink:playback_FR'
pipemixer --json list-route-rules
pipemixer enable-route-rule speakers_left off
pipemixer delete-route-rule speakers_left
pipemixer check-route-rules
pipemixer create-bus mix
pipemixer create-send speakers pipemixer.bus.mix.output my-speaker-node
pipemixer set-volume pipemixer.send.speakers.output 80
pipemixer create-effect vocal voice
pipemixer effect-params vocal
pipemixer set-effect-param vocal 'highpass:Freq' 100
pipemixer bypass-effect vocal on
pipemixer create-effect custom @/path/to/filter.conf
pipemixer --json list buses
pipemixer --json list sends
pipemixer --json list effects
pipemixer delete-send speakers
pipemixer delete-effect vocal
pipemixer delete-effect custom
pipemixer delete-bus mix
pipemixer save-scene listening
pipemixer list-scenes
pipemixer check-scene listening
pipemixer load-scene listening
pipemixer set-startup-scene listening
pipemixer get-startup-scene
pipemixer restore-startup
pipemixer set-startup-scene off
pipemixer delete-scene listening
pipemixer --json list nodes
pipemixer get-volume id:42
pipemixer set-volume id:42 55 FL
pipemixer set-mute id:42 off
pipemixer get-default sink
pipemixer set-default id:42
pipemixer list-routes id:42
pipemixer set-route id:42 1
pipemixer set-target id:80 id:42
pipemixer set-target id:80 default
pipemixer list-profiles id:25
pipemixer set-profile id:25 2
```

Targets are exact `node.name` or `device.name` values, `id:N`, or `serial:N` for nodes. For `set-target`, STREAM must be a playback or recording node, and DESTINATION must be a sink or source node of the corresponding kind. `list` shows the names and IDs to use. Volume is a percentage from 0 to 150; the optional channel uses names such as `FL` and `FR`. Query commands print text by default and accept `--json`. Mutation commands print nothing on success and wait for PipeWire to report the requested state. `--timeout MS` changes the default 5000 ms deadline.

Exit status is 0 on success, 1 on a PipeWire error or timeout, 2 for invalid arguments, and 3 when the target, destination, channel, route, or profile is unavailable. CLI commands do not require a config file unless one is given with `--config`.

`list ports` and `list links` describe the actual audio graph, including monitor
ports and filter nodes. `list graph` returns nodes, ports, and links together;
with `--json` it returns an object containing three arrays. MIDI/video ports and
pipemixer's internal peak-meter streams are omitted. Object IDs are valid only
for the current PipeWire session.
`connect` and `disconnect` accept exact `node.name:port.name` values or `id:N`
from `list ports`. The first port must be an audio output and the second an
audio input. Both commands are idempotent and wait for PipeWire to report the
result. MIDI and video connections are not supported.

Managed names contain 1–48 ASCII letters, digits, hyphens or underscores.
`create-bus NAME` exports `pipemixer.bus.NAME.input` (sink) and
`pipemixer.bus.NAME.output` (source). `create-send NAME SOURCE DESTINATION`
accepts node names, IDs or serials; the source must have audio output ports and
the destination audio input ports. Its separate playback stream is
`pipemixer.send.NAME.output`. `create-effect NAME eq|voice|empty|@CONFIG_FILE`
exports `pipemixer.effect.NAME.input` and `.output`. Reusing an existing name
fails; deletion is idempotent. Send creation waits for its actual links and
uses fixed targets with no fallback to another device.

`effect-params` lists the current value, range and default of each DSP control;
JSON also reports whether it is writable. `set-effect-param` applies a finite
value within that range without recreating the nodes or links. Frequency is
in Hz, EQ gain in dB, Q is dimensionless, and noise-gate thresholds are linear
amplitudes (0–1). Attack/hold/release use seconds. Built-in bypass switches wet
and dry branches in one parameter update. Custom chains need `wet:Mult` and
`dry:Mult` controls with the same semantics to support `bypass-effect`.

`effect-chain NAME` lists each processor's ID, type, position and bypass state.
`add-effect-stage NAME TYPE [POSITION]` inserts a processor (by default at the
end), `move-effect-stage NAME STAGE POSITION` reorders it, and
`remove-effect-stage NAME STAGE` removes it. Positions start at 1.
`bypass-effect-stage NAME STAGE on|off` switches that processor's dry/wet path.
Types are `lowshelf`, `peaking`, `highshelf`, `highpass`, `lowpass`, `gain`, `gate`,
`compressor`, `limiter`.
Parameter names use the processor ID, for example `gain1:Mult`.

```sh
pipemixer create-effect processing empty
pipemixer add-effect-stage processing gain
pipemixer set-effect-param processing gain1:Mult 0.5
pipemixer add-effect-stage processing highpass 1
pipemixer effect-chain processing
pipemixer bypass-effect-stage processing gain1 on
```

The compressor provides `Threshold dB`, `Ratio`, `Attack ms`, `Release ms`,
`Knee dB`, `Makeup dB` and `Mix` controls. The limiter provides `Ceiling dB`,
`Release ms` and `Input dB`; place it after processors that increase gain.
Both use independent detectors for the two stereo channels. The limiter has
no lookahead and limits digital sample peaks; it does not measure intersample
true peaks. Its ceiling applies at that processor's output, so later effects
and output gain can increase the final level.

```sh
pipemixer add-effect-stage processing compressor
pipemixer set-effect-param processing compressor1:'Threshold dB' -24
pipemixer set-effect-param processing compressor1:Ratio 4
pipemixer add-effect-stage processing limiter
pipemixer set-effect-param processing limiter1:'Ceiling dB' -1
pipemixer save-scene studio
```

`./build.sh` builds `pipemixer` and `pipemixer-dynamics.so`. Deploy both;
`./deploy.sh` uploads them together. The worker searches beside its executable
and in the installed PipeMixer library directory for the bundled plugin.

Custom files contain a PipeWire `filter.graph` object, as in
[the equalizer](assets/effects/eq.conf) and [voice preset](assets/effects/voice.conf).
The graph and optional editable-chain definition are loaded; stream properties
come from pipemixer. Config files
are limited to 1 MiB and any referenced plugins must be available on the
machine running pipemixer. For custom controls, reset uses the plugin's default.
The board integration suite and PCM probe are documented in [tests](tests/README.md).

Scene files are versioned JSON in `$XDG_CONFIG_HOME/pipemixer/scenes/NAME.json`,
or `$HOME/.config/pipemixer/scenes/NAME.json` when `XDG_CONFIG_HOME` is unset.
Saving the same name replaces the file atomically. Files use mode 0600 and new
directories mode 0700. `list-scenes`, `check-scene` and `delete-scene` work without
PipeWire; `check-scene` validates the file and its desired graph, including
feedback detection, without applying it.

Loading recreates missing managed paths, reuses matching paths, restores their
state and connections, and removes extra connections whose two endpoints are
both in the scene. Other managed paths are retained. A missing external node,
ambiguous name, conflicting existing path, changed channel layout or invalid
DSP value is reported instead of silently choosing another device. Application
streams referenced by a scene must also be running. Version 2 snapshots also
save hardware device/profile names and per-node route names. Loading selects
the saved profile first, waits for recreated hardware nodes, then selects the
route before applying gains and connections. Version 1 scenes remain readable
and keep their previous behavior without hardware mode changes. Save an older
scene again to include its current hardware Profile/Route. Loading has a default timeout of 30 seconds and accepts
`--timeout MS`. Scene loads are serialized; successful workers survive TUI exit.
New snapshots retain automatic link ownership, so restored rule connections
can still be removed on disable or target switching. Older snapshots without
ownership fields restore their connections as manual links.
If a runtime failure occurs, newly created paths are removed; changes already
applied to existing paths may require reloading the scene after fixing the
reported problem.

## Diagnostics, audio history and multitrack recording

Press `i` for diagnostics of the focused node (or the selected matrix source).
Each channel shows sample peak, RMS, a two-second peak hold, overload sample
counts and nonfinite sample counts. Enter resets the meter counters and recent
events. The panel also shows process/system CPU, RSS, available RAM and live
PipeWire quantum/rate, processing and scheduling time, DSP load and xrun counts.
Unavailable profiler measurements are marked explicitly. The bounded event log
contains recent graph errors and observed xrun increases. Measurements are sample
based; integrated LUFS, intersample true peaks and spectral analysis are not included.

Press `R` to manage audio history and multitrack recording. Choose **Create audio
history**, toggle sources with Enter/Space, then choose a 10- or 30-second cache.
The focused node is preselected. The session page exports the latest five seconds
or the whole cache, starts/stops recording, and shows CPU, RAM, disk space,
missing sources and capture drops. Exports and recording controls run asynchronously.
Closing the menu or TUI keeps completed caches and recordings running. **Stop cache**
finalizes any active recording and releases that session's buffers.

```sh
pipemixer list nodes
pipemixer --json diagnostics 1000
pipemixer --json meter pipemixer.bus.mix.output 1000
pipemixer start-history rehearsal 10 pipemixer.bus.mix.output
pipemixer --json list-history
pipemixer --json history-status rehearsal
pipemixer export-history rehearsal /mnt/recordings/recent-take 5
pipemixer record-history rehearsal /mnt/recordings/full-take 3
pipemixer stop-recording rehearsal
pipemixer stop-history rehearsal
```

`start-history NAME SECONDS NODE...` accepts 1–8 distinct sources and 1–120
seconds, subject to a **64 MiB per-session budget** including ring buffers and
capture queues and a runtime allowance. RSS also includes PipeWire runtime
allocations and is reported separately. Admission leaves a 64 MiB available-RAM reserve. IDs and
serial selectors are resolved to stable node names at creation. Multiple sources
produce separate **stereo 48 kHz, 32-bit float WAVs**, with conversion by PipeWire.
A common monotonic frame timeline and a 100 ms capture guard keep exported track
lengths aligned. Missing sources produce silence and reconnect under the same
stable name. This timeline does not provide a shared hardware word clock across
independent devices. Capture queue drops and missing frames remain visible.
Only the bounded cache is kept in memory; it writes no audio to disk until an
export or recording is requested. Internal capture streams are hidden from the
mixer, routing matrix and scenes.

Export/recording destinations must be **new directories** under an existing
parent; existing directories and files are never overwritten. A take contains
`track01-part0001.wav`, one file per source/segment, and `session.json` with
source names, frame range, gap/drop counts and segment details. `export-history`
exports up to the requested seconds (default: the whole configured cache), or the
shorter available history. `record-history` starts continuous recording with an
optional cache preroll (default: zero, at most the configured cache duration).
`stop-recording` finalizes files while leaving the cache running. Stop continuous
recording before exporting history so an export to slow storage cannot stall
that recording. The rolling cache continues during recording.

Recordings use 60-second segments, checkpoint headers/metadata once per second,
and finalize on explicit stop or SIGINT/SIGTERM. The writer runs separately from
audio callbacks. Low disk space stops recording and reports an error while the
cache continues; at least **16 MiB of free disk space** is reserved. Configure
`PIPEMIXER_RECORD_RESERVE_MB=16..1024` and
`PIPEMIXER_RECORD_SEGMENT_SECONDS=1..3600` before starting a cache to change these
limits. Status reports bytes, elapsed recording time, disk free space, process
CPU/RSS and per-track availability, drops and gaps. Power loss or SIGKILL can lose
the latest checkpoint interval; graceful finalization requires normal stop.

TUI takes are placed under `$XDG_STATE_HOME/pipemixer/recordings`, defaulting to
`~/.local/state/pipemixer/recordings`. Use a mounted storage device or the CLI to
select another destination on boards with small root filesystems. One stereo
track writes 384,000 bytes/s; eight tracks write 3,072,000 bytes/s before metadata.
`/tmp` is suitable for tests and consumes RAM on this board. Cache sessions and
active recording state last for the current audio session; recordings are not
automatically started after reboot, and completed files remain on disk.

## Independent monitors and Solo

Monitor definitions are saved atomically with mode 0600 in
`$XDG_CONFIG_HOME/pipemixer/monitors/NAME.json` (otherwise under `~/.config`).
Names use 1–32 ASCII letters, digits, hyphens or underscores. Up to 32 monitors
and 32 normal/Solo sources per monitor are supported. Use exact stable node
names; numeric IDs and serial selectors are rejected. Definition edits and
`check-monitors` work without PipeWire. `list-monitors` observes the live graph;
`--json` includes engine status, normal/active sources, Solo, listening source,
mode, connection counts and `connected`, `switching`, `waiting`, `blocked`,
`conflict` or `disabled` state.

```sh
pipemixer create-monitor headphones usb-headphones \
  pipemixer.bus.music.output pipemixer.bus.voice.output
pipemixer --json list-monitors
pipemixer solo-monitor headphones pipemixer.bus.voice.output toggle
pipemixer clear-monitor-solo headphones
pipemixer set-monitor-source headphones pipemixer.bus.music.output
pipemixer set-monitor-source headphones mix
pipemixer set-monitor-sources headphones pipemixer.bus.music.output
pipemixer set-volume pipemixer.monitor.headphones.output 50
pipemixer set-mute pipemixer.monitor.headphones.output on
pipemixer enable-monitor headphones off
pipemixer enable-monitor headphones on
pipemixer check-monitors
pipemixer delete-monitor headphones
```

The supervised `routing-daemon` creates `pipemixer.monitor.NAME.input`/`.output`
and maintains stereo routes independently of the main mix. Solo takes priority
over the single listening source, which takes priority over the normal mix.
The normal mix and listening choice remain stored while Solo is active. Use
`set-monitor-sources NAME off` for an empty normal mix, and
`solo-monitor NAME NODE on|off|toggle` for each Solo source. Missing selected
sources stay silent and reconnect under their stable names; there is no
automatic substitution. Closing the TUI leaves monitoring active. The board
service reloads definitions after audio-server restart or reboot, even with
startup scene restoration disabled.

Each config directory owns its monitor paths and connections. A monitor with
the same name in another config reports a conflict. Unmanaged connections into
the monitor input also report a conflict and suspend the managed output until
those connections are removed. Disabling removes managed monitor routes;
deletion removes the definition and its own path. Source gain/mute and main
connections remain intact. Names `monitor_NAME_NUMBER` are reserved for that
monitor's internal routing rules. Scenes capture the monitor path, its gains
and connection ownership; the persistent monitor policy supplies the current
normal mix, Solo and listening choice after a scene load.

## Startup restoration on the Luckfox board

Upload the built binary, its companion `pipemixer-dynamics.so` and the complete
`scripts/` directory to the board. Keep the plugin beside the binary. Upload
`assets/automation/` and `docs/` beside `scripts/` to install the example and guide. Run
the installer on the board from that uploaded directory:

```sh
sh scripts/install-board-startup /path/to/built/pipemixer
export XDG_RUNTIME_DIR=/run/user/0
/usr/local/bin/pipemixer save-scene boot
/usr/local/bin/pipemixer set-startup-scene boot
/usr/local/bin/pipemixer get-startup-scene
```

The installer places the plugin in `/usr/local/lib/pipemixer/`. If it is in a
different upload directory, pass its path as the installer's second argument.

The installer places the binary in `/usr/local/bin`, the supervisor in
`/usr/local/libexec`, and the SysV services at `/etc/init.d/S98pipemixer` and
`/etc/init.d/S99pipemixer-automation`. It updates the normal `/usr/bin/pipemixer`
entry and backs up the previous entry as `pipemixer.before-local-install`.
The launcher finds an existing `/run/user/UID` session when XDG_RUNTIME_DIR is unset. These
locations and `/root/.config/pipemixer` survive board reboots. It starts
PipeWire and WirePlumber when they are absent and restores the selected scene
once per audio session. The service continues running with the TUI closed.
Its logs are `/tmp/pipemixer-session.log`, `/tmp/pipemixer-pipewire.log`, and
`/tmp/pipemixer-wireplumber.log`; logs under `/tmp` are cleared on reboot.

`set-startup-scene` validates the saved file before selecting it and works
without PipeWire. Selection is stored atomically in
`$XDG_CONFIG_HOME/pipemixer/startup-scene` (with the same HOME fallback as
scenes). The special value `off` disables restoration. Deleting the selected
scene also disables it. Selection changes take effect at the next audio
session; `restore-startup` applies the selection immediately.
After changing the setup, save it under the selected name again to update
the state that will be restored next time.

Startup restoration selects available saved hardware profiles first, then
skips absent external devices/application streams and
sends that depend on them, while restoring independent buses, effects and
the remaining connections. It logs the number of skipped nodes and leaves
the saved scene intact. Conflicting names, incompatible layouts and invalid
parameters still fail. The background routing engine then restores devices/applications when they
appear: channel volumes, mute, stream targets, defaults, dependent sends and
saved manual connections. It restores each new object generation once, while
keeping live changes on endpoints that remained present. Sends destroyed with
their external endpoints are recreated with their saved gains. Tagged rule and
monitor links follow the current policies, including fallback and Solo.
Reappearing hardware devices restore the saved Profile/Route by name before
node parameters and connections. Unsupported names report blocked; temporary
unavailability waits for the device capability to return. A manual profile or
route change on an already restored object is retained until scene reload or
the next device/node generation.
A short confirmation window handles session-manager initialization; persistent
conflicts are reported instead of continually forcing parameters.
Ordinary `load-scene` continues to require all referenced external nodes.

A successful `load-scene` or `restore-startup` also activates recovery of that
loaded snapshot for the current audio session. `save-scene` updates the saved
file; load it again to activate the updated recovery snapshot. The engine keeps
a private ledger under XDG_RUNTIME_DIR, so restarting the engine preserves
live adjustments. `recovery-status` (also `--json`) shows the active scene and
waiting/restoring/restored/blocked devices and nodes, including route and
stream-target progress. `clear-recovery` disables this replay
without changing startup selection, routing rules or running audio. Deleting
the active scene disables its replay; `restore-startup` with startup off also
clears replay. For retrying a blocked restore, fix the conflict and load the
scene or run `restore-startup` again.

```sh
pipemixer load-scene studio
pipemixer recovery-status
pipemixer --json recovery-status
pipemixer clear-recovery
```

```sh
/usr/local/bin/pipemixer set-startup-scene off
/etc/init.d/S98pipemixer status
```

The service also accepts `start`, `stop` and `restart`. Stopping it shuts down
audio servers that it started itself. On other distributions, integrate
`pipemixer restore-startup` after the existing PipeWire/session-manager
startup instead of installing this root SysV service.

## Automatic routing and endpoint reconnection

The board audio service runs `pipemixer routing-daemon` after startup scene
restoration. It keeps enabled port connections alive with the TUI closed,
reloads rule changes once per second, and restarts the engine after an audio
server restart or engine exit. Run `routing-daemon` in the foreground when
using an existing PipeWire session on another system. One engine is allowed
per config directory. The board engine log is `/tmp/pipemixer-routing.log`.

`create-route-rule NAME OUTPUT_PORT INPUT_PORT [OPTIONS]` saves an enabled rule
using `node.name:port.name` values from `list ports`. Matching defaults to exact,
so version 1 files and literal names retain their behavior. It works when the
endpoints or PipeWire are absent, so devices can be configured before they
are plugged in. Names use the same 1–48 character rules as managed paths.
Existing names are protected against replacement. `enable-route-rule NAME
on|off`, `delete-route-rule NAME`, and `check-route-rules` work offline.

`list-route-rules` queries live PipeWire state. JSON returns
`{"engine_running":true,"rules":[...]}`; each rule includes its stable
endpoints, enabled flag, state and reason. States are `waiting`, `connecting`,
`connected`, `partial`, `suppressed`, `disabled`, `ambiguous`, `blocked` (feedback), and `error`.
An absent endpoint stays waiting. Ambiguous names and feedback loops prevent
connection. JSON also reports matched port counts, selected/connected pair
counts, priority, exclusive group, suppressed pairs, fallback choices and
`active_input`. The last is null when no target is selected or different
sources use different targets; `using_fallback` is true if any source uses a
backup. `list links` shows individual endpoints.

`--match glob` enables shell-style `*`, `?` and `[abc]` patterns with backslash
escaping. Quote patterns in the shell. All matching output/input combinations
with the same `audio.channel` connect, allowing multiple sources and targets
without crossing FL/FR. Unlabelled ports connect only when both sides have
one match. No implicit mono/stereo conversion is added. New matching ports
join automatically. Limits are 256 matches per side, 1024 pairs per rule and
8192 pairs per rule set; excess expansion reports an error.

`--priority N` sets -1000000..1000000 (default 0). Higher numbers win within
`--exclusive-group NAME`; ties use ascending rule names. Each source node has
its own winner, and a candidate must cover all matching channels of that
source. Unavailable/blocked candidates let a lower rule take over. Ungrouped
rules keep fan-out. Groups govern this engine's owned connections; manual
connections and other rule sets retain their ownership. Use
`set-route-priority NAME N [GROUP|off]` to edit a rule offline. Omit the last
argument to keep the group; `off` clears it.

Repeat `--fallback INPUT_PORT` to define up to eight ordered backup inputs
using the rule's match mode. Each source node selects the first candidate
with compatible channels and viable connections. Its channels switch together.
Missing targets, feedback and failed negotiation trigger the next candidate.
A better target must stay viable for `--switch-delay MS` before switching
back (0..60000, default 1000); failures switch away immediately. Failed
negotiations have a five-second cooldown. The engine removes its old
connections before creating replacement links. Gains, mute and DSP parameters
keep their existing values.

```sh
pipemixer create-route-rule monitor 'my-source:*' 'usb-speakers:playback_*' \
  --match glob --priority 100 --exclusive-group listening \
  --fallback 'internal-speakers:playback_*' --fallback 'headphones:playback_*' \
  --switch-delay 1500
pipemixer set-route-priority monitor 120
pipemixer set-route-fallbacks monitor 'internal-speakers:playback_*' --switch-delay 2000
pipemixer set-route-fallbacks monitor off
```

`set-route-fallbacks NAME INPUT...|off [--switch-delay MS]` replaces the backup
list; delay-only edits retain the list. Duplicate selectors (including the
primary) are rejected. New advanced rules use version 2 JSON, adding `match`,
`priority`, `exclusive_group`, `fallbacks` and `switch_delay_ms` to the fields
shown below. These settings survive restart.

Files live in `$XDG_CONFIG_HOME/pipemixer/rules/NAME.json`, with the same HOME
fallback as scenes. They are versioned JSON, atomically written with mode
0600. The engine retains the last valid rules when a reload fails and logs
the error. Example:

```json
{
  "format": "pipemixer.route-rule",
  "version": 1,
  "name": "speakers_left",
  "enabled": true,
  "output": "my-source:capture_FL",
  "input": "my-sink:playback_FL"
}
```

Connections created by the engine carry rule and config-directory ownership
tags. Disabling or deleting a rule removes its own connections; existing
manual connections and connections owned by another rule set are retained.
Stopping the engine leaves connections alive. Disabling a rule while the
engine is stopped takes effect when it next starts. Enabled rules repair
manually disconnected links, so disable a rule to stop maintaining its pair.

Rules are shared across scenes in the same config directory. The engine
waits during scene loading, then resumes maintaining its enabled connections.
Set their enabled states to match the routing policy for the selected scene.
The engine changes port connections; existing node gains, mute and DSP
parameters keep their current values. Missing buses/effects/sends can be
recreated by loading their scene. Devices appearing later can then connect
to those paths through the rules. Exact rules describe one port pair; glob
rules maintain a batch of pairs. The same engine replays hardware modes and
returning device state from the loaded scene, while rules select connections
according to their current priorities and fallback targets.

## Advanced automation and MIDI/OSC

Press `o` to manage the automation engine, fade the focused node or an effect
parameter, cancel fades, inspect jobs, and enable/run persistent rules.
The background engine supports combined conditions, RMS hysteresis, hold/release
and cooldown, priorities, parallel action sequences, scenes and periodic triggers.
MIDI 1.0 RawMIDI supports CC, notes and program changes; OSC UDP supports direct
controls and configured rule mappings. Network listening requires configuration.

```sh
pipemixer fade-volume NODE_NAME 50 1500 smooth
pipemixer fade-effect-param EFFECT_NAME 'compressor1:Threshold dB' -30 2000
pipemixer cancel-fade
pipemixer import-automation /path/to/automation.json
pipemixer start-automation
pipemixer automation-status
pipemixer stop-automation
```

Definitions live in `~/.config/pipemixer/automation.json` (or XDG_CONFIG_HOME).
The board's separate `S99pipemixer-automation` service supervises enabled engines
after scene restoration. Start/stop preferences persist; transient job progress
is not replayed. Direct fades stop on manual takeover or a replaced target.
See the [complete usage guide](docs/AUTOMATION.md) and
[editable configuration example](assets/automation/example.json).
MIDI integration has been tested with a virtual character device; attached
controller hardware is still needed for physical MIDI validation.

## Config
pipemixer reads its config from `$XDG_CONFIG_HOME/pipemixer/pipemixer.ini`.
See [example config](assets/pipemixer.ini) and pipemixer.ini(5) for details.

## References
- https://docs.pipewire.org
- https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/master/src/tools
- https://github.com/saivert/pwvucontrol
- https://github.com/quickshell-mirror/quickshell/tree/master/src/services/pipewire
- https://invisible-island.net/ncurses
- https://tldp.org/HOWTO/NCURSES-Programming-HOWTO

[pipewire]: https://pipewire.org/
[pulsemixer]: https://github.com/GeorgeFilipkin/pulsemixer
[pwvucontrol]: https://github.com/saivert/pwvucontrol
[ncurses]: https://invisible-island.net/ncurses
