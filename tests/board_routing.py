"""Board integration tests for port connections and the interactive matrix."""
import fcntl
import os
import pty
import select
import struct
import subprocess
import termios
import time

import board_graph as graph


class Tui:
    def __init__(self, term='xterm-256color'):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        self.log = open('/tmp/board/routing-test.log', 'w')
        self.process = subprocess.Popen([graph.BINARY, '-l', 'trace', '-L', '2'],
                                        stdin=slave, stdout=slave, stderr=self.log,
                                        env=dict(graph.ENV, TERM=term))
        os.close(slave)
        self.drain(.5)

    def drain(self, seconds=.2):
        result = bytearray()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if select.select([self.master], [], [], max(0, end - time.monotonic()))[0]:
                try:
                    result.extend(os.read(self.master, 65536))
                except OSError:
                    break
        assert self.process.poll() is None, 'TUI exited: ' + open('/tmp/board/routing-test.log').read()
        return bytes(result)

    def send(self, keys):
        os.write(self.master, keys)
        return self.drain()

    def resize(self, rows, columns):
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        os.kill(self.process.pid, 28)
        return self.drain()

    def close(self):
        os.write(self.master, b'q')
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError('TUI did not exit')
        assert self.process.returncode == 0
        os.close(self.master)
        self.log.close()


def sorted_ports(direction):
    return sorted((p for p in graph.query('ports') if p['direction'] == direction),
                  key=lambda p: (p['node_name'], p['name'], p['id']))


def links_between(output, inp):
    return [l for l in graph.query('links') if l['output']['port_id'] == output and l['input']['port_id'] == inp]


def main():
    names = [graph.PREFIX + name for name in ['a', 'b', 'c']]
    ui = None
    try:
        for name in names:
            graph.create_sink(name)
        ports = graph.query('ports')
        def port(name, direction, channel='FL'):
            return next(p for p in ports if p['node_name'] == name and p['direction'] == direction and p['channel'] == channel)
        out = port(names[0], 'output')
        inp = port(names[1], 'input')
        dest2 = port(names[2], 'input')
        for _ in range(3):
            graph.run(graph.BINARY, 'connect', 'id:' + str(out['id']), 'id:' + str(inp['id']))
        assert len(links_between(out['id'], inp['id'])) == 1
        graph.run(graph.BINARY, 'connect', out['node_name'] + ':' + out['name'], dest2['node_name'] + ':' + dest2['name'])
        assert links_between(out['id'], dest2['id'])
        reverse = port(names[1], 'output')
        home = port(names[0], 'input')
        result = graph.run(graph.BINARY, 'connect', 'id:' + str(reverse['id']), 'id:' + str(home['id']), check=False)
        assert result.returncode == 1 and 'loop' in result.stderr.lower(), result
        assert graph.run(graph.BINARY, 'connect', 'id:' + str(inp['id']), 'id:' + str(out['id']), check=False).returncode == 3
        assert graph.run(graph.BINARY, 'connect', 'id:4294967294', 'id:' + str(inp['id']), check=False).returncode == 3
        for target in [inp, dest2]:
            graph.run(graph.BINARY, 'disconnect', 'id:' + str(out['id']), 'id:' + str(target['id']))
            graph.run(graph.BINARY, 'disconnect', 'id:' + str(out['id']), 'id:' + str(target['id']))
        graph.reference_check()
        print('PASS connect/disconnect, fan-out, idempotency, feedback and invalid port checks', flush=True)

        ui = Tui()
        assert b'Routing matrix' in ui.send(b'r')
        outputs, inputs = sorted_ports('output'), sorted_ports('input')
        row = next(i for i, p in enumerate(outputs) if p['id'] == out['id'])
        column = next(i for i, p in enumerate(inputs) if p['id'] == inp['id'])
        ui.send(b'g' + b'j' * row + b'l' * column)
        ui.send(b' ')
        graph.wait_for(lambda: links_between(out['id'], inp['id']))
        ui.send(b'\n')
        graph.wait_for(lambda: not links_between(out['id'], inp['id']))
        print('PASS matrix keyboard connects and disconnects actual ports', flush=True)

        # Mouse cell position follows the matrix's documented scrolling layout.
        visible_rows, columns = 17, 8
        row_scroll = max(0, row - visible_rows + 1)
        col_scroll = max(0, column - columns + 1)
        x = 40 + (column - col_scroll) * 7 + 3
        y = 3 + row - row_scroll + 1
        ui.send(('\x1b[<0;%d;%dM\x1b[<0;%d;%dm' % (x, y, x, y)).encode())
        graph.wait_for(lambda: links_between(out['id'], inp['id']))
        ui.resize(8, 30)
        ui.resize(40, 140)
        ui.resize(24, 100)
        graph.remove_sink(names[1])
        ui.drain(.4)
        assert not any(l['input']['node_name'] == names[1] for l in graph.query('links'))
        graph.create_sink(names[1])
        ui.drain(.4)
        ui.send(b'r')
        ui.send(b'r')
        ui.send(b'\x1b')
        ui.close()
        ui = None
        print('PASS mouse, resize, endpoint removal/reappearance and matrix exit', flush=True)
    finally:
        if ui:
            ui.close()
        for name in reversed(names):
            graph.remove_sink(name)
    graph.reference_check()
    print('STAGE 2 PASSED', flush=True)


if __name__ == '__main__':
    main()
