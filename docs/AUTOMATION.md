# 第6项：高级自动化与外部控制

音量与连续效果参数可以渐变；条件规则、周期触发、MIDI 和 OSC 共用动作引擎。
规则定义和启停设置保存在配置目录，任务进度与触发计数属于当前工作进程。

## TUI 操作

按 `o` 打开自动化菜单。可以启动/停止引擎、渐变当前节点音量、渐变当前效果的参数、
取消渐变、查看近期任务，以及启停和手动执行规则。可以从混音器和 `r` 接线台打开。
先选中要控制的节点；选中效果输入或输出节点后，可以打开效果参数列表。

在渐变编辑页，`j/k` 选择项目，`h/l` 调整目标值、时长和曲线，选择 **执行渐变**（英文 **Run fade**）后
按 Enter 执行。菜单每 250 ms 刷新状态，退出 TUI 后已启动的渐变继续执行。
复杂条件与 MIDI/OSC 映射通过 `automation.json` 编辑和导入；本轮 TUI 提供执行、
启停、参数渐变与状态管理，尚未提供条件树的图形编辑器。

## 直接渐变

```sh
export XDG_RUNTIME_DIR=/run/user/0
pipemixer --json list nodes
pipemixer fade-volume pipemixer.bus.music.output 50 1500 smooth
pipemixer fade-effect-param voice_fx 'compressor1:Threshold dB' -30 2000 linear
pipemixer cancel-fade pipemixer.bus.music.output
pipemixer cancel-fade voice_fx 'compressor1:Threshold dB'
pipemixer cancel-fade
pipemixer automation-status
pipemixer --json automation-status
```

目标使用实际 `node.name`，效果参数使用 `effect-params NAME` 给出的名称。
直接音量渐变也接受 `id:N` 或 `serial:N`，开始时固定到当前对象。
渐变过程中设备消失、同名对象替换、声道布局改变或手动修改该参数，任务停止并
记录原因。CLI/TUI 手动编辑先停止同一配置作用域的对应渐变，并确认旧写入已送达，
再写手动值，防止途中写入覆盖手动接管。其他客户端的修改通过回报值检测。
取消后保留当时的值。同一目标/参数的新渐变替换原渐变。
场景加载或效果链编辑持锁时暂停写入；参数或对象改变、或持续持锁超过约 100 ms
时取消渐变，避免覆盖恢复的参数。后台恢复的短暂持锁仅暂停当前更新。

时长为 20～3600000 ms，音量范围 0～150%。音量保持声道相对比例，最大声道到达
目标百分比；初始全静音时，各声道从同一目标恢复。百分比沿用现有立方音量标度：
50% 对应实际增益 0.125。`linear` 对实际增益线性插值，`smooth` 使用缓入缓出曲线。
效果参数按其数值插值，并检查处理器范围；整数和布尔参数使用即时参数动作。
调度间隔约 20 ms，每轮最多写入一个渐变控制，按任务轮转，合计最多约 50 次/秒。
单个渐变通常每 20 ms 更新；8 个同时运行时，每个约 160 ms 更新。曲线按实际经过
时间计算，并写入准确终点，结束确认会受并发数量和服务端回复影响。这样限制
WirePlumber 的参数缓存刷新开销，避免通知积压阻塞新查询。本轮没有逐采样点包络。
渐变保留节点的 mute 设置，解除 mute 后才可听到被静音节点的音量变化。

## 配置、持久化与后台恢复

配置位于 `$XDG_CONFIG_HOME/pipemixer/automation.json`，未设置 XDG 时位于
`~/.config/pipemixer/automation.json`。示例见
[example.json](../assets/automation/example.json)。示例规则默认关闭，先修改实际节点名。
板子安装时也会复制示例到 `/usr/local/share/pipemixer/automation/example.json`，
中文说明保存在 `/usr/local/share/doc/pipemixer/AUTOMATION.md`。
安装脚本也将日常 `/usr/bin/pipemixer` 入口指向新版本，并保存旧入口为
`/usr/bin/pipemixer.before-local-install`。未指定运行目录且板上当前用户已有
PipeWire 会话时，入口自动使用 `/run/user/UID`，可以直接运行 `pipemixer`。

```sh
pipemixer check-automation /path/to/automation.json
pipemixer import-automation /path/to/automation.json
pipemixer start-automation
pipemixer enable-automation voice_duck on
pipemixer trigger-automation voice_duck
pipemixer stop-automation
```

导入先验证，再以 0600 权限原子保存并同步文件和目录。运行中的引擎每 250 ms 检查
变化；坏文件保留上一份有效定义并显示 `config_error`。有效重载取消旧定义的排队
动作与渐变。删除配置停止条件规则，直接启动的渐变仍可继续。

`start-automation` 将 `automation-enabled` 保存为 `on`，`stop-automation` 保存为
`off` 并停止任务。已安装板子的 `/etc/init.d/S99pipemixer-automation` 负责监督引擎，
等待场景恢复和路由引擎启动后再自动启动，进程异常退出后重启；保存为 off 后保持停止。
首次已有配置且没有偏好文件时，默认允许自动启动。单独执行 `fade-volume` 会自动
启动一个后台工作进程，不会把该瞬时渐变保存为重启任务。

```sh
/etc/init.d/S99pipemixer-automation status
pipemixer automation-daemon   # 需要手工监督时的前台入口
```

重启加载规则与启停设置，触发计数重新开始。渐变、等待和动作队列不会恢复到中途状态。
`on_start` 默认为 false：已有条件先建立基线，之后状态改变才执行。
设为 true 的条件规则可以在启动/重载时执行首次满足的条件，仍遵守 hold 和 cooldown。
手动 `trigger-automation` 执行该规则的 actions，跳过条件检查，遵守 enabled、优先级和冷却。

本板 PipeWire 1.6.8 的元数据解绑缺陷会在客户端过早断开后丢失后续通知，影响默认
设备确认和条件规则。兼容模块回移植了 [PipeWire 上游修复](https://github.com/PipeWire/pipewire/commit/06de0ed2b191070c5048f7e7e7b0e190d1f35fbf)。
项目保留补丁及重建脚本；需要已编译的相同 Buildroot SDK：

```sh
python3 scripts/build-pipewire-metadata-fix
# 将生成的模块及安装脚本传到板子，测试通过后在板子执行：
sh scripts/install-pipewire-metadata-fix /path/to/libpipewire-module-metadata.so
```

该专用安装脚本核对本板原模块的 SHA-256，保存 `.before-pipemixer` 备份，并重启
音频服务。通用 `install-board-startup` 不修改 PipeWire 模块。其他镜像应使用已含
该修复的 PipeWire，或在对应版本中回移植补丁。

## 带状态的条件规则

```json
{
  "format": "pipemixer.automation",
  "version": 1,
  "rules": [
    {
      "name": "voice_duck",
      "enabled": true,
      "priority": 100,
      "on_start": true,
      "hold_ms": 150,
      "release_ms": 500,
      "cooldown_ms": 300,
      "when": {
        "all": [
          {"type":"present", "target":"pipemixer.bus.voice.output", "value":true},
          {"type":"level", "target":"pipemixer.bus.voice.output", "above":-35, "hysteresis":4}
        ]
      },
      "actions": [
        {"type":"fade-volume", "target":"pipemixer.bus.music.output", "value":50, "duration_ms":200, "curve":"smooth"}
      ],
      "otherwise": [
        {"type":"fade-volume", "target":"pipemixer.bus.music.output", "value":100, "duration_ms":700, "curve":"smooth"}
      ]
    }
  ]
}
```

条件持续满足 `hold_ms` 后执行 actions；持续不满足 `release_ms` 后执行 otherwise。
release 默认等于 hold，两者默认 0。`cooldown_ms` 限制连续状态动作的间隔，期间保持
的最新状态会在冷却后执行。`above`/`below` 为严格比较，迟滞让已进入状态的退出阈值
偏移指定值，避免阈值附近反复切换。

| 条件 | 字段与含义 |
| --- | --- |
| AND / OR / NOT | `{"all":[...]}` / `{"any":[...]}` / `{"not":{...}}` |
| 设备/节点存在 | `type:"present", target:node.name, value:true/false` |
| 静音 | `type:"mute", target:node.name, value:true/false` |
| 音量 | `type:"volume", target:node.name, above:60, hysteresis:5`，最大声道百分比 |
| 效果参数 | `type:"parameter", target:effect_name, parameter:"compressor1:Ratio", above:3` |
| 电平 | `type:"level", target:node.name, above:-35, hysteresis:4`，最大声道 RMS dBFS |
| 连接 | `type:"link", target:"source:port", input:"sink:port", value:true/false` |
| 默认设备 | `type:"default", target:node.name, parameter:"sink"/"source"` |
| 启动后时间 | `{"elapsed_ms":10000}`，相对本次引擎启动的单调时间 |

缺失参数、没有可用音频样本等返回“不可用”，NOT 也保留不可用状态。
AND 中明确为假的条件、OR 中明确为真的条件仍能决定结果。因此示例里显式加入
present 条件，语音源消失时可解除音乐压低；只写电平条件时，源消失会等待有效数据。
规则使用稳定名称，返回的同名设备重新被观察。

较高数值优先级的已生效规则抑制低优先级规则对同一音量、静音或参数的操作；
低优先级条件仍成立时，可在高优先级条件解除后执行。同优先级不互相抑制，按规则
名称排序处理条件规则，外部事件按接收顺序排队。独立规则的渐变/等待并行推进。
动作按顺序执行，遇到失败停止该组并显示错误；已经完成的动作保留其结果。
即时静音和效果参数写入等待 PipeWire 确认；同一控制上新执行的动作取代仍在确认的
旧动作，避免快速 MIDI 按下/松开使用过期状态。不同控制的动作仍独立推进。
场景加载使用现有排他锁，其间其他队列等待。

| 动作 type | 字段 |
| --- | --- |
| `volume` | `target:node.name, value:percent` |
| `mute` | `target:node.name, value:true/false` |
| `parameter` | `target:effect_name, parameter:parameter_name, value:number` |
| `fade-volume` | `target:node.name, value:percent, duration_ms:ms, curve:"linear"/"smooth"` |
| `fade-parameter` | `target:effect_name, parameter:parameter_name, value:number, duration_ms:ms` |
| `wait` | `duration_ms:ms` |
| `scene` | `target:scene_name`，完成加载后执行后续动作 |
| `route-rule` | `target:route_rule_name, value:true/false`，持久启停路由规则 |
| `monitor-source` | `target:monitor_name, parameter:source_node_name`，`off` 恢复正常来源 |
| `monitor-solo` | `target:monitor_name, parameter:source_node_name`，`off` 清除 Solo |

最多 32 条规则、256 个条件节点、8 层条件嵌套，每组 actions/otherwise 最多 8 个动作，
最多 16 组排队/执行动作和 32 个渐变。配置最大 256 KiB，重复键、非法 JSON、NUL 名称、
无效范围和未知字段会被拒绝。持续等待的动作有超时，状态页展示 failed、holding、
cooldown、suppressed、unavailable、queued、applying、active/idle 等状态。

## OSC UDP

只有配置 osc 才打开端口，`port:0` 关闭。默认 bind 为 127.0.0.1，port 为 9000；
从电脑控制板子时，显式改为板子 IPv4 地址或 `0.0.0.0`。`direct:false` 只接受配置的
规则映射；`direct:true` 额外接受下面的直接控制接口。

```json
"osc": {"bind":"0.0.0.0", "port":9000, "direct":true}
```

| 地址 | 参数，括号内可选 |
| --- | --- |
| `/pipemixer/volume` | string 节点名、float/int 百分比、(int ms、string 曲线) |
| `/pipemixer/mute` | string 节点名、int 0/1 |
| `/pipemixer/parameter` | string 效果名、string 参数名、float/int 值、(int ms、string 曲线) |
| `/pipemixer/scene` | string 场景名 |
| `/pipemixer/rule` | string 规则名，通过 enabled/条件/hold/冷却/优先级检查后执行 |

使用 OSC 1.0 的大端数字和 4 字节对齐字符串格式；支持数值/字符串消息和 timetag=1
的即时 bundle，最多 16 条消息、4 层 bundle、4096 字节。畸形 bundle 在执行前整体
拒绝；方法执行错误不回滚之前的方法。未来时标和地址通配符尚未实现。
响应为 `/pipemixer/reply`，参数为 string 请求地址、int 结果码、string 错误信息；
0 表示请求已接受，渐变/场景的最终结果查看 automation-status。每秒最多接受 200
个包，超出计入 dropped。网络输入、队列和连接错误也在 TUI/JSON 状态中可见。
格式参考 [OSC 1.0 原始规范](https://opensoundcontrol.stanford.edu/spec-1_0.html)。

下列 Python 示例可直接在电脑发送，不需要安装 OSC 库。TARGET 换成实际节点名：

```python
import socket, struct

def osc_string(value):
    data = value.encode() + b'\0'
    return data + b'\0' * (-len(data) % 4)

target = 'pipemixer.bus.music.output'
packet = (osc_string('/pipemixer/volume') + osc_string(',sfis')
          + osc_string(target) + struct.pack('>fi', 50.0, 1500)
          + osc_string('smooth'))
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
    client.settimeout(2)
    client.sendto(packet, ('192.168.123.100', 9000))
    print(client.recv(2048))
```

规则映射接受一个 0～1 的 float/int 参数，也可不带参数（输入值为 0）：

```json
{
  "name": "controller_music",
  "trigger": {"osc":"/controller/music"},
  "actions": [{"type":"fade-volume", "target":"pipemixer.bus.music.output",
               "value":"input", "min":0, "max":100, "duration_ms":100}]
}
```

## MIDI 1.0 RawMIDI 与周期触发

接上 MIDI 控制器后查看 `/dev/snd/midi*`，配置实际字符设备路径。也可以使用板子已有
的稳定设备符号链接。每秒重试缺失或断开的设备，重连时清空 MIDI 解析状态。
RawMIDI 的接口背景见 [ALSA 官方文档](https://www.alsa-project.org/alsa-doc/alsa-lib/rawmidi.html)。

```json
"midi": {"device":"/dev/snd/midiC1D0"}
```

```json
{
  "name": "midi_fader",
  "trigger": {"midi":{"type":"cc", "channel":1, "number":7}},
  "actions": [{"type":"fade-volume", "target":"pipemixer.bus.music.output",
               "value":"input", "min":0, "max":100, "duration_ms":100}]
}
```

`type` 支持 `cc`、`note`、`program`，channel 为 1～16，number 为 0～127。
note 的 `edge` 为 press（默认）、release、any，velocity=0 的 Note On 按松开处理。
CC 输入是 value/127，音符按下是 velocity/127，松开是 0。Program Change 匹配线上
0～127 的编号，一些控制器的屏幕会显示成 1～128。可以把 Program Change 动作改为
`{"type":"scene","target":"live"}` 切换场景。

`value:"input"` 可以映射音量、效果参数或 mute；音量/参数要求明确的 min/max，
mute 以输入 >=0.5 为 true。要让任何力度的音符都切换 mute，使用 press/release
两条规则分别执行固定的 true/false。MIDI/OSC 规则的 when 可省略；提供 when 时
它成为事件触发的条件门，hold/cooldown 仍生效。

解析支持 running status、消息间的实时字节、音符松开和 SysEx 跳过。
本轮接入 RawMIDI 字节流，不包含 MIDI 2.0、ALSA Sequencer 路由或 SysEx 参数协议。
板子当前未连接 MIDI 硬件；已用虚拟字符设备验证解析、映射和断线重连，实物测试待接入设备。

周期事件使用单调时间，100～86400000 ms，错过周期后从当前时间重新计时：

```json
{
  "name": "periodic_action",
  "trigger": {"interval_ms":5000},
  "when": {"type":"present", "target":"pipemixer.bus.music.output", "value":true},
  "actions": [{"type":"parameter", "target":"music_fx", "parameter":"compressor1:Ratio", "value":3}]
}
```

事件规则不使用 otherwise/on_start；通过独立的松开映射或条件规则表达恢复动作。
