# pipemixer
This is a TUI volume control application for [pipewire] built with [ncurses].
Heavily inspired by [pulsemixer] and [pwvucontrol].

![Screenshot](assets/screenshot.png)

The channel rows show a live peak meter in dBFS beside the volume setting.
Meters monitor only the selected tab and decay after playback stops.

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
pipemixer --json list nodes
pipemixer get-volume id:42
pipemixer set-volume id:42 55 FL
pipemixer set-mute id:42 off
pipemixer get-default sink
pipemixer set-default id:42
pipemixer list-routes id:42
pipemixer set-route id:42 1
pipemixer list-profiles id:25
pipemixer set-profile id:25 2
```

Targets are exact `node.name` or `device.name` values, `id:N`, or `serial:N` for nodes. `list` shows the names and IDs to use. Volume is a percentage from 0 to 150; the optional channel uses names such as `FL` and `FR`. Query commands print text by default and accept `--json`. Mutation commands print nothing on success and wait for PipeWire to report the requested state. `--timeout MS` changes the default 5000 ms deadline.

Exit status is 0 on success, 1 on a PipeWire error or timeout, 2 for invalid arguments, and 3 when the target, channel, route, or profile is unavailable. CLI commands do not require a config file unless one is given with `--config`.

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

