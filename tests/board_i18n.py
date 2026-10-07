"""Chinese/English curses UI, Unicode geometry and unchanged control identifiers.

Run on the board with PIPEMIXER_BINARY selecting the candidate/installed binary.
Uses isolated configuration/state directories and virtual audio paths.
"""
import json
import locale
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

import board_graph as graph
from board_routing import sorted_ports
from board_terminal import ViewTui

BASE = Path('/tmp/board/i18n-fixture')
INI = BASE / 'config/pipemixer/pipemixer.ini'
MIX, FX, MON = 'test_i18n_mix', 'test_i18n_fx', 'test_i18n_monitor'
DELETED = 'test_i18n_delete'
TARGET = 'pipemixer.bus.' + MIX + '.output'
SOURCE = graph.PREFIX + 'i18n_中文来源'
DESTINATION = graph.PREFIX + 'i18n_destination'
F2 = b'\x1bOQ'


def command(*args, check=True):
    result = subprocess.run([graph.BINARY, *args], env=graph.ENV,
                            text=True, capture_output=True, timeout=35)
    if check and result.returncode:
        raise AssertionError((args, result.returncode, result.stderr))
    return result


def data(*args):
    return json.loads(command('--json', *args).stdout)


def effect_stages():
    result = command('--json', 'effect-chain', FX, check=False)
    return json.loads(result.stdout) if result.returncode == 0 else []


def history_status(name):
    result = command('--json', 'history-status', name, check=False)
    return json.loads(result.stdout) if result.returncode == 0 else {}


def configure(language):
    INI.parent.mkdir(parents=True, exist_ok=True)
    INI.write_text('[main]\nlanguage=' + language + '\n', encoding='utf-8')


def close(ui):
    if ui:
        ui.close()


def selected_source(ui, name):
    ui.send(b'r')
    index = next(i for i, p in enumerate(sorted_ports('output')) if p['node_name'] == name)
    ui.send(b'g' + b'j' * index)


def live_frame(ui, expected):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if expected in ui.screen.text:
            return
        ui.drain(.15)
    raise AssertionError((expected, ui.screen.text))


def main():
    before = graph.query()
    old_environment = dict(graph.ENV)
    original_locale = locale.setlocale(locale.LC_CTYPE)
    utf8 = None
    for candidate in ['C.UTF-8', 'en_US.UTF-8', 'zh_CN.utf8']:
        try:
            locale.setlocale(locale.LC_CTYPE, candidate)
            utf8 = candidate
            break
        except locale.Error:
            pass
    locale.setlocale(locale.LC_CTYPE, original_locale)
    assert utf8, 'A UTF-8 locale is required for the board UI test'
    assert not BASE.exists(), 'Refusing to replace an existing fixture directory'
    graph.ENV.update(XDG_CONFIG_HOME=str(BASE / 'config'), XDG_STATE_HOME=str(BASE / '中文录音'),
                     LANG='C', LC_CTYPE=utf8)
    graph.ENV.pop('LC_ALL', None)
    graph.ENV.pop('LC_MESSAGES', None)
    graph.ENV.pop('PIPEMIXER_LANGUAGE', None)
    ui, daemon = None, None
    try:
        configure('zh_CN')
        for language in ['auto', 'en', 'zh_CN', 'zh-CN']:
            configure(language)
            command('--config', str(INI), '--validate')
        configure('bad-language')
        assert command('--config', str(INI), '--validate', check=False).returncode == 1
        configure('zh_CN')
        graph.run('pw-cli', 'create-node', 'adapter',
                  '{ factory.name=support.null-audio-sink node.name="' + SOURCE + '"'
                  ' node.description="中文鼠标测试" media.class=Audio/Sink'
                  ' object.linger=true audio.position=[FL FR] priority.session=0 }')
        graph.wait_for(lambda: any(n['name'] == SOURCE for n in graph.query()['nodes']))
        graph.create_sink(DESTINATION)
        command('create-bus', MIX)
        command('create-effect', FX, 'empty')
        command('add-effect-stage', FX, 'compressor')

        ui = ViewTui()
        assert '播放流' in ui.screen.text and '输入设备' in ui.screen.text, ui.screen.text
        # Chinese Output Devices begins at column 18, not byte offset 24.
        ui.send(b'\x1b[<0;19;1M\x1b[<0;19;1m')
        assert '中文鼠标测试' in ui.screen.text, ui.screen.text
        ui.send(b'\x1b[<0;41;1M\x1b[<0;41;1m')
        assert '模式：' in ui.screen.text, ui.screen.text
        ui.send(F2)
        assert 'Profiles:' in ui.screen.text and '模拟立体声' not in ui.screen.text, ui.screen.text
        ui.send(F2)
        ui.send(b'3')
        for key, label in [(b'b', '音频总线与独立发送'), (b's', '场景'),
                           (b'e', '效果链'), (b'a', '自动路由'),
                           (b'M', '独立监听'), (b'i', '专业诊断'),
                           (b'R', '音频历史与多轨录音'), (b'o', '自动化 / MIDI / OSC')]:
            ui.send(key)
            live_frame(ui, label)
            assert '\ufffd' not in ui.screen.text, ui.screen.text
            ui.send(b'\x1b')
        ui.send(b'sg\n')
        graph.wait_for(lambda: 'scene1' in command('list-scenes').stdout)
        command('check-scene', 'scene1')
        ui.send(b'rvv')
        assert '设备' in ui.screen.text, ui.screen.text
        ui.send(b'/')
        ui.send('中文来源'.encode())
        ui.send(b'\n')
        assert '中文来源' in ui.screen.text, ui.screen.text
        ui.send(b'/\x15\n')
        ui.send(b'gz')
        assert '+ 设备' in ui.screen.text, ui.screen.text
        ui.send(F2)
        assert '+ Devices' in ui.screen.text, ui.screen.text
        ui.send(F2)
        assert '+ 设备' in ui.screen.text, ui.screen.text
        ui.send(b'r')
        ui.send(b'F')  # Unbound printable key remains harmless.
        ui.send(F2)
        assert 'Playback' in ui.screen.text and 'Output Devices' in ui.screen.text, ui.screen.text
        ui.send(b's')
        assert 'Save current setup' in ui.screen.text, ui.screen.text
        ui.send(F2)
        assert '播放流' in ui.screen.text, ui.screen.text
        for dimensions in [(12, 40), (8, 24), (24, 100)]:
            ui.resize(*dimensions)
            ui.send(b's')
            # Menu frame must survive wide text clipping on the top border.
            assert ui.screen.cells[2][1] == '┌', ui.screen.text
            assert ui.screen.cells[2][dimensions[1] - 2] == '┐', ui.screen.text
            ui.send(b'\x1b')
        close(ui); ui = None
        print('PASS Chinese panels, mouse tab geometry, search, scenes, F2 and narrow terminal borders', flush=True)

        ui = ViewTui(); ui.send(b'5e')
        effects = sorted((n for n in graph.query()['nodes'] if n['kind'] == 'effect' and n['role'] == 'input'),
                         key=lambda n: n['id'])
        index = 3 + next(i for i, n in enumerate(effects) if n['group'] == FX)
        ui.send(b'g' + b'j' * index + b'\n')
        live_frame(ui, '压缩器')
        ui.send(b'\n')
        assert '阈值 dB' in ui.screen.text, ui.screen.text
        ui.send(b'h')
        graph.wait_for(lambda: next(p['value'] for p in data('effect-params', FX)
                                   if p['name'] == 'compressor1:Threshold dB') == -19)
        ui.send(b'\x1ba')
        assert '峰值限幅器' in ui.screen.text and '(limiter)' in ui.screen.text, ui.screen.text
        ui.send(b'g' + b'j' * 5 + b'\n')
        graph.wait_for(lambda: len(effect_stages()) == 2)
        live_frame(ui, 'gain1')
        live_frame(ui, '效果链已更新')
        close(ui); ui = None

        daemon = subprocess.Popen([graph.BINARY, 'routing-daemon'], env=graph.ENV,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        graph.wait_for(lambda: data('list-route-rules')['engine_running'])
        command('create-monitor', MON, DESTINATION, SOURCE, TARGET)
        graph.wait_for(lambda: data('list-monitors')['monitors'][0]['state'] == 'connected')
        ui = ViewTui(); selected_source(ui, SOURCE); ui.send(b'M\n')
        live_frame(ui, '编辑正常混音来源')
        ui.send(b'\x1bS')
        graph.wait_for(lambda: SOURCE in data('list-monitors')['monitors'][0]['solo'])
        ui.send(b'LU')
        graph.wait_for(lambda: data('list-monitors')['monitors'][0]['mode'] == 'listen')
        close(ui); ui = None
        print('PASS translated effect parameters retain CLI identifiers; Chinese monitor/Solo controls work', flush=True)

        command('set-volume', TARGET, '100')
        ui = ViewTui(); selected_source(ui, TARGET); ui.send(b'o')
        ui.send(b'gj\n')
        assert '渐变编辑' in ui.screen.text, ui.screen.text
        ui.send(b'gjj' + b'h' * 10 + b'gjjj' + b'l' * 8)
        ui.send(b'g\n')
        graph.wait_for(lambda: data('automation-status')['active_fades'] == 1)
        ui.send(F2)
        assert 'Playback' in ui.screen.text, ui.screen.text
        graph.wait_for(lambda: abs(data('get-volume', TARGET)['channels'][0]['percent'] - 50) < .01)
        ui.send(F2)
        ui.send(b'R\n')
        live_frame(ui, '选择来源')
        ui.send(b'g\n')
        graph.wait_for(lambda: len(data('list-history')) == 1)
        live_frame(ui, '/ 10 秒')
        ui.drain(.4)  # Wait for the startup child before issuing detail-page keys.
        history = data('list-history')[0]['name']
        ui.send(b'gjj\n')
        graph.wait_for(lambda: history_status(history).get('recording') is True)
        ui.drain(.7)
        ui.send(b'gjj\n')
        graph.wait_for(lambda: history_status(history).get('recording') is False)
        files = list((BASE / '中文录音').glob('pipemixer/recordings/*/track01-part*.wav'))
        assert files, 'No WAV produced by the Chinese recording UI'
        header = files[0].read_bytes()
        riff, size, wave = struct.unpack('<4sI4s', header[:12])
        assert (riff, wave) == (b'RIFF', b'WAVE') and size + 8 == len(header) and len(header) > 1000
        ui.send(b'gjjj\n')
        live_frame(ui, '完成所有录音收尾')
        ui.send(b'j\n')
        graph.wait_for(lambda: not data('list-history'))
        close(ui); ui = None
        print('PASS Chinese fade continues through F2; multitrack recording uses unchanged commands and Unicode paths', flush=True)

        command('create-bus', DELETED)
        ui = ViewTui(); ui.send(b'5b')
        row = next(i for i, line in enumerate(ui.screen.text.splitlines())
                   if DELETED in line and '删除' in line)
        ui.send(b'g' + b'j' * (row - 3) + b'\n')
        graph.wait_for(lambda: not any(n['group'] == DELETED for n in graph.query()['nodes']))
        close(ui); ui = None
        print('PASS managed bus deletion uses the original kind identifier in the Chinese menu', flush=True)

        # Environment override, locale auto detection, and explicit C fallback.
        configure('auto')
        graph.ENV['LANG'] = 'zh_CN.utf8'
        graph.ENV['LC_CTYPE'] = utf8
        ui = ViewTui()
        assert '播放流' in ui.screen.text, ui.screen.text
        close(ui); ui = None
        graph.ENV['PIPEMIXER_LANGUAGE'] = 'en'
        ui = ViewTui()
        assert 'Playback' in ui.screen.text, ui.screen.text
        close(ui); ui = None
        english = data('list', 'graph')
        graph.ENV['PIPEMIXER_LANGUAGE'] = 'zh_CN'
        assert data('list', 'graph') == english
        graph.ENV['LC_ALL'] = 'C'
        ui = ViewTui(); ui.send(F2)
        assert 'language remains English' in ui.screen.text, ui.screen.text
        close(ui); ui = None
        print('PASS language config, locale auto/override, byte-locale fallback and stable JSON output', flush=True)
    finally:
        if ui:
            Path('/tmp/board/i18n-last-screen.txt').write_text(ui.screen.text, encoding='utf-8')
            try:
                close(ui)
            except AssertionError:
                pass  # Clean private fixtures even when an earlier UI check failed.
        command('stop-automation', check=False)
        for entry in data('list-history'):
            command('stop-history', entry['name'], check=False)
        command('delete-monitor', MON, check=False)
        if daemon:
            if daemon.poll() is None:
                graph.wait_for(lambda: not any(n['kind'] == 'monitor' and n['group'] == MON
                                              for n in graph.query()['nodes']))
            daemon.terminate()
            daemon.wait(timeout=5)
        command('delete-effect', FX, check=False)
        command('delete-bus', MIX, check=False)
        command('delete-bus', DELETED, check=False)
        graph.remove_sink(SOURCE)
        graph.remove_sink(DESTINATION)
        shutil.rmtree(BASE, ignore_errors=True)
        graph.ENV.clear(); graph.ENV.update(old_environment)
    after = graph.query()
    assert {n['name'] for n in before['nodes']} == {n['name'] for n in after['nodes']}
    assert {l['id'] for l in before['links']} == {l['id'] for l in after['links']}
    graph.reference_check()
    print('BOARD I18N PASSED', flush=True)


if __name__ == '__main__':
    main()
