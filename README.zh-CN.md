# pipemixer 中文总体说明

[English](README.md) · [自动化详细说明](docs/AUTOMATION.md) · [板级验证记录](tests/BOARD_VALIDATION.md)

pipemixer 是基于 PipeWire 和 ncurses 的终端音频管理工具。除了音量与设备控制，
当前版本还提供音频接线台、虚拟混音总线、独立监听、可编辑效果链、场景恢复、
自动路由、音频历史、多轨录音，以及 MIDI/OSC 自动化控制。

本文对应截至 **2026-10-06** 的项目实现。我们约定的六项阶段目标已经完成相应
实现、板级验证和部署；后续扩展及尚未覆盖的实物测试列在文末。
界面支持简体中文和英文，覆盖主界面、接线台、场景、路由、监听、效果链、
诊断、录音及自动化面板；中文设备名称和搜索也可正常显示和使用。

## 功能概览

| 功能             | 当前支持                                                                    |
| ---------------- | --------------------------------------------------------------------------- |
| 基础控制         | 各声道音量、静音、默认输入/输出、应用流目标、声卡 Profile 和设备 Route      |
| 场景与重启恢复   | 保存/加载音频布局和参数，开机重建路径，恢复迟到设备及应用的状态             |
| 自动路由         | 精确或 glob 批量匹配、声道配对、互斥优先级、备用设备、延迟回切、重连修复    |
| 接线台           | 实时连线、节点/类型分组、折叠、分类筛选、搜索、多选批量连接与断开           |
| 混音与监听       | 立体声虚拟总线、独立发送、独立监听混音、多源 Solo、监听源切换               |
| 效果链           | 添加/删除/排序、实时调参、单级/整链旁路，最多 16 个处理器                   |
| 专业监测         | 各声道采样峰值/RMS、峰值保持、过载计数、性能、xrun 和近期异常               |
| 音频历史与录音   | 有界滚动缓存、历史 WAV 导出、预录、分段多轨录音、空间保护                   |
| 自动化与外部控制 | 渐变、组合条件、迟滞/冷却/优先级、多步与并行动作、MIDI 1.0 RawMIDI、OSC UDP |

## 快速开始

板子已经安装新版，可以登录后直接启动：

```sh
ssh root@192.168.123.100
pipemixer
```

也可以从电脑直接打开远程 TUI：

```sh
ssh -t root@192.168.123.100 pipemixer
```

工具需要运行中的 PipeWire 会话和 WirePlumber 等会话管理器。已安装的板级服务
负责启动和监督音频会话；启动入口会在未设置 `XDG_RUNTIME_DIR` 时查找当前用户
已有的 `/run/user/UID` 会话。直接调用二进制或手动调试 root 会话时可明确指定：

```sh
export XDG_RUNTIME_DIR=/run/user/0
/usr/local/bin/pipemixer
```

默认五个标签页依次为：播放流、录音流、输出设备、输入设备、声卡。
播放/录音页显示应用流，设备页显示硬件与虚拟音频节点。
音量旁显示当前标签页的实时峰值，单位为 dBFS；停止播放后电平逐步回落。

第一次使用时，先查看节点与端口，再建立路径，最后保存场景：

```sh
pipemixer list nodes
pipemixer list ports
pipemixer --json list graph
```

下文命令中的 `YOUR_*` 是占位名称，需要替换为查询得到的真实名称。
各段命令展示对应功能，用时选择所需操作；尚未创建的总线、效果或来源需要先创建。

## 界面语言

按 `F2` 在简体中文和英文之间切换。切换时关闭当前菜单，可重新打开；
接线台的筛选、分组和折叠保持，正在运行的音频、渐变及录音继续运行。
这个切换只影响当前 TUI，不写入配置。

临时指定语言：

```sh
PIPEMIXER_LANGUAGE=zh_CN pipemixer
PIPEMIXER_LANGUAGE=en pipemixer
```

永久使用中文，编辑 `~/.config/pipemixer/pipemixer.ini`（板上 root 用户为
`/root/.config/pipemixer/pipemixer.ini`），在已有的 `[main]` 中设置：

```ini
[main]
language=zh_CN
```

可选值为 `auto`（默认）、`en`、`zh_CN`；`zh-CN` 和 `zh` 也可指定中文。
`PIPEMIXER_LANGUAGE` 优先于 INI。`auto` 按 `LC_ALL`、`LC_MESSAGES`、`LANG`
依次选择，中文 locale 使用简体中文，其余使用英文。
中文需要 UTF-8 locale 和终端字体支持；显式设置 `LC_ALL=C` 时保留英文界面。
语言只改变界面显示，CLI 命令、JSON 字段、节点名称和效果参数标识仍保持原值。

## 常用快捷键

以下为默认配置，**大小写有区别**。部分按键在不同面板中有不同作用。

`F2` 在所有 TUI 面板中切换语言，可在 `[binds]` 中用 `toggle-language=f2`
重新指定按键；功能键名称支持 `f1`～`f12`。

| 位置          | 按键                                | 操作                                       |
| ------------- | ----------------------------------- | ------------------------------------------ |
| 主界面        | `j/k` 或上下方向键                | 移动焦点                                   |
| 主界面        | `h/l` 或左右方向键                | 减小/增大音量                              |
| 主界面        | `m`                               | 切换静音                                   |
| 主界面        | `Space`                           | 切换声道联动                               |
| 主界面        | `Tab` / `Shift+Tab`，或 `t/T` | 切换标签页                                 |
| 主界面        | `1`～`5`                        | 直接选择标签页                             |
| 主界面        | `g/G`                             | 跳到首项/末项                              |
| 主界面        | `c`                               | 为当前播放/录音流选择目标设备              |
| 主界面        | `p`                               | 选择设备 Route/端口                        |
| 主界面        | `P`                               | 选择声卡 Profile                           |
| 主界面        | `D`                               | 将选中的输入/输出设备设为默认              |
| 主界面        | `r`                               | 打开接线台                                 |
| 主界面        | `b`                               | 管理总线、发送及受管音频路径               |
| 主界面        | `e`                               | 创建效果或编辑当前效果链                   |
| 主界面        | `s`                               | 场景保存、加载、删除及开机场景选择         |
| 主界面        | `a`                               | 管理自动路由规则                           |
| 主界面/接线台 | `M`                               | 管理独立监听                               |
| 主界面/接线台 | `S` / `L` / `U`               | 当前来源 Solo / 监听该来源 / 清除全部 Solo |
| 主界面/接线台 | `i`                               | 当前节点的专业诊断                         |
| 主界面/接线台 | `R`                               | 管理音频历史和多轨录音                     |
| 主界面/接线台 | `o`                               | 自动化管理、渐变与任务状态                 |
| 菜单          | `Enter` / `Esc`                 | 确认 / 返回或取消                          |
| 主界面        | `q`                               | 退出 TUI                                   |

鼠标默认启用：点击标签或行切换焦点，点击音量条设置音量，点击音量数值切换静音，
滚轮滚动列表。右键应用流选择设备，右键输入/输出设备设置默认；中键节点切换静音。
可在 INI 的 `[main]` 中设置 `mouse=false` 关闭鼠标捕获。

## 音频对象与名称

PipeWire 中，节点代表设备、应用流或处理路径，端口承载具体声道，连接把输出端口
接到输入端口。对象 ID 会在重启或重建后变化；场景、路由和监听使用稳定名称。

| 对象         | 作用                                            | 创建后的主要节点名称                           |
| ------------ | ----------------------------------------------- | ---------------------------------------------- |
| 总线 Bus     | 接收多路音频并输出混音                          | `pipemixer.bus.NAME.input` / `.output`     |
| 发送 Send    | 将来源复制到另一个设备或总线，拥有独立音量/静音 | `pipemixer.send.NAME.output`                 |
| 效果 Effect  | 接收音频并输出处理后的信号                      | `pipemixer.effect.NAME.input` / `.output`  |
| 监听 Monitor | 建立独立的监听混音与输出路径                    | `pipemixer.monitor.NAME.input` / `.output` |

总线和效果的 `.input` 是可接收应用播放的虚拟输出设备，`.output` 是承载结果的
虚拟输入设备。创建总线或效果后，需要通过应用目标、发送或接线台接入音频和目标设备。

总线、发送、效果和路由规则名称使用 1～48 个 ASCII 字母、数字、连字符或下划线；
监听名称最多 32 个字符。已存在的名称不能重复创建。

### 虚拟混音总线与独立发送

按 `b` 创建总线，或从当前来源创建独立发送。也可以使用 CLI：

```sh
pipemixer create-bus music
pipemixer create-bus voice
pipemixer set-target YOUR_PLAYBACK_STREAM pipemixer.bus.music.input
pipemixer create-send speakers pipemixer.bus.music.output YOUR_SPEAKER_NODE
pipemixer set-volume pipemixer.send.speakers.output 80
pipemixer set-mute pipemixer.send.speakers.output off
```

多个应用可以送入同一个总线。一个来源也可以建立多个发送，分别调整扬声器、
录音或其他总线的接收电平。发送使用指定目标；备用设备策略由自动路由规则提供。

路径创建完成后会给出提示。关闭 TUI 不会停止已创建的后台音频路径；删除路径使用
`b` 菜单或 `delete-send`、`delete-effect`、`delete-bus`。

## 接线台与批量操作

按 `r` 打开实时路由矩阵。行是输出端口，列是输入端口；用方向键或 `hjkl`
选择单元格，按 `Enter`、`Space` 或鼠标点击建立/断开连接。

| 标记  | 含义     |
| ----- | -------- |
| `+` | 已连接   |
| `.` | 未连接   |
| `~` | 正在协商 |
| `!` | 出错     |

| 接线台按键      | 操作                                                    |
| --------------- | ------------------------------------------------------- |
| `v`           | 切换平铺、按节点、按类型分组                            |
| `z/Z`         | 折叠当前输出组/输入组                                   |
| `f`           | 按设备、总线、发送、效果或应用筛选                      |
| `/`           | 搜索；`Enter` 应用，`Esc` 取消，`Ctrl+U` 清空输入 |
| `x/X`         | 选择输出端口或组 / 输入端口或组                         |
| `n/N`         | 选择当前输出节点 / 输入节点的全部声道                   |
| `c/d`         | 批量连接 / 断开所选匹配声道                             |
| `u`           | 清除批量选择                                            |
| `a`           | 将当前端口对保存为精确自动路由规则                      |
| `A`           | 将当前两节点的兼容声道保存为批量规则                    |
| `?`           | 查看接线台操作提示                                      |
| `r` / `Esc` | 返回主界面；有批量任务时`Esc` 可取消任务              |

批量操作按声道名称配对，例如 FL 对 FL、FR 对 FR；没有声道名称时，仅支持双方
各一个匹配端口的情况。当前没有隐式单声道/立体声转换，也不支持 MIDI/视频接线。
整批建链前检查反馈环路；建链失败或取消时只撤销本次新建的连接。
批量断开若被取消，可能保留部分已完成的断开结果。

折叠不会清除选择；应用新的搜索或分类筛选会清除选择。启用的自动路由规则可能
重新建立手动断开的连接，要长期断开对应路径，应先停用相应规则。

## 场景、开机恢复与设备状态恢复

按 `s` 保存当前布局、加载/删除场景，或选择开机恢复的场景。TUI 自动使用
`scene1`、`scene2` 等可用名称；CLI 可以指定名称。

场景保存总线、发送、效果定义及顺序、参数和旁路、各声道音量、静音、应用流目标、
默认输入/输出及连接。版本 2 还保存硬件 Profile/Route 的稳定名称；版本 1 仍可加载。

```sh
pipemixer save-scene studio
pipemixer list-scenes
pipemixer check-scene studio
pipemixer load-scene studio
pipemixer set-startup-scene studio
pipemixer get-startup-scene
```

同名保存会原子替换文件。`check-scene` 校验文件及期望连接图，包含反馈检查，
不会应用场景。加载会重建缺失的受管路径、复用匹配路径，恢复参数及连接；
两个端点都属于场景的额外连接会被清理，其他受管路径保留。

普通 `load-scene` 要求引用的外部设备和应用流存在；名称歧义、配置冲突、声道布局
变化或参数不合法会报告错误。加载默认超时 30 秒，可使用 `--timeout MS` 修改。
运行时失败会清理本次新建路径，已改变的现有路径可能需要修复问题后重新加载场景。

开机恢复会先恢复可用硬件 Profile，再恢复总线、效果和其他路径。缺席的外部设备、
应用流及依赖发送暂时跳过，后台引擎等待它们出现后恢复音量、静音、目标、默认设备、
硬件 Route 和连接。每个新对象恢复一次，持续存在的节点保留现场调整。

```sh
pipemixer --json recovery-status
pipemixer restore-startup
pipemixer clear-recovery
pipemixer set-startup-scene off
```

`restore-startup` 立即应用所选开机场景；`off` 关闭下一次开机/音频会话的场景恢复。
`clear-recovery` 关闭当前已加载快照的设备状态重放。
成功加载场景会激活该快照的重连恢复；只执行 `save-scene` 更新的是磁盘文件，
需要再次加载，才会更新当前使用的恢复快照。

**场景、自动路由规则和监听策略分开持久保存。** 场景提供音频路径与参数，路由规则
决定当前优先级和备用目标，监听策略提供正常混音、Solo 与监听选择。
加载场景后，自动连接继续遵循当前规则和监听策略。

## 自动路由、优先级与备用设备

主界面按 `a` 查看规则状态、启停或删除；菜单中 `h/l` 或左右键每次调整优先级 10。
接线台中 `a` 保存一对端口，`A` 保存两节点之间的兼容声道。

后台 `routing-daemon` 在关闭 TUI 后继续维护连接。板级服务在场景恢复后启动它，
并在音频会话重启或引擎异常退出后重新启动。

```sh
pipemixer create-route-rule speakers_left 'YOUR_SOURCE:capture_FL' 'YOUR_SPEAKER:playback_FL'
pipemixer create-route-rule speakers_right 'YOUR_SOURCE:capture_FR' 'YOUR_SPEAKER:playback_FR'
pipemixer --json list-route-rules
pipemixer check-route-rules
pipemixer enable-route-rule speakers_left off
```

高级规则可以一次维护多个来源和目标，并配置备用设备：

```sh
pipemixer create-route-rule output_policy 'YOUR_SOURCE:*' 'YOUR_PRIMARY:playback_*' \
  --match glob --priority 100 --exclusive-group main_output \
  --fallback 'YOUR_BACKUP:playback_*' --switch-delay 1500
pipemixer set-route-priority output_policy 120
pipemixer set-route-fallbacks output_policy 'YOUR_BACKUP:playback_*' --switch-delay 2000
```

- 精确匹配为默认模式。glob 支持 `*`、`?` 和 `[abc]`，模式需要加引号，避免 shell 展开。
- 批量匹配按 `audio.channel` 配对，新出现的匹配端点自动加入。
- 互斥组内按来源节点分别选择最高优先级的可用规则；同优先级按规则名升序。
- 未设置互斥组的规则支持同时分发到多个目标。
- 每条规则最多八个备用目标，按顺序选择声道兼容且能正常连接的目标。
- 目标缺席或协商失败时转向备用；更优目标持续可用达到等待时间后回切，默认 1000 ms。
- 规则只清理自身拥有的连接，手动连接和其他规则作用域的连接保留。

规则定义可以离线创建和校验。端点暂时缺席显示 `waiting`，名称歧义、反馈或配置
冲突会报告对应状态。规则文件损坏时，运行中的引擎保留上一份有效规则。

## 独立监听、Solo 与监听源切换

按 `M` 创建或选择监听，编辑正常混音来源、Solo 来源和单一监听源，启停或删除监听。
从接线台创建时，先选来源输出和耳机/监听目标输入，菜单会预选它们。

每个监听拥有独立路径、音量和静音；操作监听不会改变来源音量，也不会改变主输出
或录音通路的连接。监听优先顺序为：**Solo → 单一监听源 → 正常混音**。
多个 Solo 来源可以同时混合；清除 Solo 后恢复此前的监听源或正常混音。
选定来源缺席时保持静音，出现后按稳定名称重新连接。

```sh
pipemixer create-monitor headphones YOUR_HEADPHONE_NODE \
  pipemixer.bus.music.output pipemixer.bus.voice.output
pipemixer set-volume pipemixer.monitor.headphones.output 50
pipemixer solo-monitor headphones pipemixer.bus.voice.output on
pipemixer clear-monitor-solo headphones
pipemixer set-monitor-source headphones pipemixer.bus.music.output
pipemixer set-monitor-source headphones mix
pipemixer --json list-monitors
```

监听定义使用真实稳定节点名称，不接受 ID 或 serial 选择器。最多支持 32 个监听，
每个监听的正常混音/Solo 来源分别最多 32 个。关闭 TUI 后监听继续，服务重启或
整板重启后恢复定义；即使开机场景设为 `off`，监听策略仍可独立恢复。
监听增益等场景参数的恢复需要相应场景。

## 可编辑效果链

按 `e` 创建均衡器 `eq`、人声预设 `voice` 或空链 `empty`。
选中效果的输入/输出节点后再次按 `e`，打开处理器列表。

| 效果链按键             | 操作                                   |
| ---------------------- | -------------------------------------- |
| `j/k`                | 选择处理器                             |
| `Enter`              | 打开参数；参数页中恢复选中参数的默认值 |
| `a` / `d`          | 添加 / 删除处理器                      |
| `K/J`                | 上移 / 下移处理器                      |
| `Space`              | 切换单个处理器旁路                     |
| `B`                  | 切换整链旁路                           |
| 参数页`h/l` 或左右键 | 修改参数                               |
| `Esc`                | 返回                                   |

当前可编辑链支持以下九类处理器，每链最多 16 个：

| 类型           | 用途           |
| -------------- | -------------- |
| `gain`       | 线性增益       |
| `highpass`   | 高通滤波       |
| `lowpass`    | 低通滤波       |
| `lowshelf`   | 低频搁架 EQ    |
| `peaking`    | 参数峰值 EQ    |
| `highshelf`  | 高频搁架 EQ    |
| `gate`       | 噪声门         |
| `compressor` | 压缩器         |
| `limiter`    | 采样峰值限幅器 |

CLI 示例：

```sh
pipemixer create-effect voice_fx empty
pipemixer add-effect-stage voice_fx highpass
pipemixer add-effect-stage voice_fx compressor
pipemixer add-effect-stage voice_fx limiter
pipemixer effect-chain voice_fx
pipemixer effect-params voice_fx
pipemixer set-effect-param voice_fx 'highpass1:Freq' 80
pipemixer set-effect-param voice_fx 'compressor1:Threshold dB' -24
pipemixer set-effect-param voice_fx 'compressor1:Ratio' 4
pipemixer set-effect-param voice_fx 'limiter1:Ceiling dB' -1
```

参数名使用稳定处理器 ID，例如 `compressor1:Ratio`，以 `effect-params` 的输出为准。
效果创建后，还需把来源接到 `pipemixer.effect.voice_fx.input`，再把
`pipemixer.effect.voice_fx.output` 接到目标；可以使用发送或接线台。

```sh
pipemixer create-send into_fx pipemixer.bus.voice.output pipemixer.effect.voice_fx.input
pipemixer create-send processed pipemixer.effect.voice_fx.output YOUR_SPEAKER_NODE
```

添加、删除和排序会短暂中断该效果链，随后恢复连接、声道音量、静音及依赖发送。
已编辑链的参数调整和旁路使用实时控制。链条顺序、参数与单级旁路保存在场景中。

压缩器提供阈值、比例、Attack/Release、软拐点、补偿增益和干湿混合。
限幅器提供 Ceiling、输入增益及 Release；两者的左右声道分别检测。
当前限幅器没有前瞻或过采样真峰值检测，其上限作用于该处理器输出，之后的增益
仍可能提高最终电平。

也可通过 `create-effect NAME @/path/to/filter.conf` 加载自定义 PipeWire
`filter.graph`，并编辑其公开参数。自定义图文件最多 1 MiB，依赖插件需安装在运行机器上。
示例见 [EQ 配置](assets/effects/eq.conf)和[人声配置](assets/effects/voice.conf)。

## 专业监测

选中节点后按小写 `i` 打开诊断；接线台中诊断当前来源。

- 各声道采样峰值、RMS、2 秒峰值保持、过载与非有限值采样计数。
- PipeWire quantum、采样率、调度/处理时间、DSP 负载和 xrun。
- 进程/系统 CPU、进程 RSS、可用内存及近期图错误和 xrun 增加事件。

不可用的 profiler 指标会明确标记。按 `Enter` 重置电平计数和近期事件。
CLI 可以指定采集时长，单位为毫秒：

```sh
pipemixer --json diagnostics 1000
pipemixer --json meter pipemixer.bus.music.output 1000
```

当前是采样峰值/RMS 监测，尚无 LUFS、过采样真峰值或 FFT 频谱分析。

## 音频历史与多轨录音

按大写 `R`，选择 **Create audio history**，用 `Enter/Space` 勾选来源，再选择
10 秒或 30 秒缓存。会话页可导出最近 5 秒或全部缓存，启动/停止录音并查看资源、
来源缺席、缺口和丢帧。导出与录音控制异步执行，关闭 TUI 后采集继续。

CLI 支持 1～8 个不同来源、1～120 秒缓存，但申请必须满足内存预算：

```sh
pipemixer start-history rehearsal 10 pipemixer.bus.music.output pipemixer.bus.voice.output
pipemixer --json history-status rehearsal
pipemixer export-history rehearsal /mnt/recordings/recent-take 5
pipemixer record-history rehearsal /mnt/recordings/full-take 3
# 录音完成后停止；滚动缓存继续运行。
pipemixer stop-recording rehearsal
# 停止缓存会完成仍在进行的录音，并释放该会话。
pipemixer stop-history rehearsal
```

`/mnt/recordings` 是示例，应替换为实际挂载的存储目录。导出/录音的父目录必须已存在，
本次 take 目录必须是新目录；已有文件或目录不会被覆盖。
`record-history` 最后一个参数是预录秒数，上例先写入最近 3 秒，再连续录音。
连续录音期间缓存继续滚动；导出同一会话的历史前，需要先停止连续录音。

每个来源生成独立的 **立体声、48 kHz、32 位浮点 WAV**。
各轨共享单调时钟帧时间轴，来源缺席时补静音，重现后继续采集。
take 包含 `track01-part0001.wav` 等文件和记录来源、分段、帧范围、缺口及丢帧的
`session.json`。当前没有内置多轨回放。

| 资源或文件策略 | 默认行为                                                        |
| -------------- | --------------------------------------------------------------- |
| 内存           | 每会话缓存/队列/运行余量预算 64 MiB，并保留 64 MiB 可用系统内存 |
| 文件分段       | 每 60 秒一段                                                    |
| 检查点         | 每秒更新 WAV 文件头和元数据                                     |
| 磁盘余量       | 保留至少 16 MiB；空间不足停止录音，缓存继续                     |
| 正常停止       | 显式停止、SIGINT 或 SIGTERM 时完成 WAV 收尾                     |

在启动缓存前设置 `PIPEMIXER_RECORD_RESERVE_MB=16..1024` 或
`PIPEMIXER_RECORD_SEGMENT_SECONDS=1..3600` 可修改磁盘预留和分段长度。
突然断电或 SIGKILL 可能丢失最近检查点区间。

TUI 默认把 take 放在 `$XDG_STATE_HOME/pipemixer/recordings`，未设置时为
`~/.local/state/pipemixer/recordings`。小容量板子应使用已挂载存储目录，或通过
`XDG_STATE_HOME`/CLI 选择位置。单个立体声来源约写入 384,000 字节/秒，八个来源
合计约 3.072 MB/秒，不含元数据。

本板 `/tmp` 是 tmpfs，测试文件占用内存。此前八来源录音测试持续约 20 秒，
CPU 为一个核心的 16.61%～19.24%，RSS 约 31.7 MiB，捕获丢帧为 0；
这些数据衡量测试时长内的采集和文件生成开销，尚未验证外部存储长时间落盘性能。
软件时间轴对齐也不等于多个独立硬件设备共享字时钟。

## 高级自动化与 MIDI/OSC

按 `o` 管理后台引擎、节点音量渐变、效果参数渐变、取消任务、启停或手动执行规则，
并查看任务和外部连接状态。任务在关闭 TUI 后继续。

```sh
pipemixer fade-volume pipemixer.bus.music.output 50 1500 smooth
pipemixer fade-effect-param voice_fx 'compressor1:Threshold dB' -30 2000 linear
pipemixer cancel-fade pipemixer.bus.music.output
pipemixer --json automation-status
```

渐变支持 `linear` 和 `smooth` 曲线，时长 20～3,600,000 ms，音量 0～150%。
音量百分比使用立方标度，50% 对应实际增益 0.125；`linear` 对实际增益线性插值。
音量渐变保持声道相对比例及 mute 设置，静音节点需要解除静音才可听到变化。
目标消失/被替换或手动接管对应参数时，渐变停止。

条件规则支持 `all/any/not`，可判断设备存在、静音、音量、效果参数、RMS 电平、
连接、默认设备和经过时间；支持迟滞、保持/释放时间、冷却、优先级及周期触发。
动作可以调整音量/参数、静音、等待、加载场景、启停路由规则、切换监听源及 Solo。
同一动作序列按顺序执行，独立序列可以并行。

复杂条件及 MIDI/OSC 映射目前通过 JSON 编辑和导入，TUI 提供运行与状态管理。
先修改[示例配置](assets/automation/example.json)中的实际节点及设备路径，
再校验、导入并启用所需规则；示例中的规则默认关闭。

```sh
pipemixer check-automation /path/to/automation.json
pipemixer import-automation /path/to/automation.json
pipemixer start-automation
pipemixer enable-automation voice_duck on
pipemixer stop-automation
```

规则定义和启停偏好持久保存。板级 `S99pipemixer-automation` 在音频恢复和路由
引擎就绪后启动已启用的自动化，异常退出后监督重启；保存为 `off` 时保持停止。
重启后触发计数重新开始，渐变、等待和动作队列不会从中途恢复。

| 外部控制 | 当前支持                                                                                  |
| -------- | ----------------------------------------------------------------------------------------- |
| MIDI     | MIDI 1.0 RawMIDI，CC、Note、Program Change，通道过滤、按下/松开、running status、断线重连 |
| OSC      | UDP 直接控制音量/静音/参数/场景/规则，或配置地址映射；请求回复、即时 bundle               |

OSC 监听需要在 JSON 中配置，默认监听地址为本机。电脑远程控制板子时，需指定
可从局域网访问的监听地址、端口及控制模式。MIDI 需要填写实际 RawMIDI 设备路径。
配置格式和电脑发送 OSC 的完整示例见[中文自动化说明](docs/AUTOMATION.md)。

渐变使用控制参数更新，合计最多约 50 次写入/秒；单路通常 20 ms 更新一次，
八路并发时每路约 160 ms。终点按实际经过时间计算，当前没有逐采样点包络。
MIDI 尚无 MIDI 2.0、ALSA Sequencer 或 SysEx 参数协议；OSC 尚无未来时标调度或地址通配符。

## 命令行与脚本集成

不带子命令启动 TUI，带子命令执行非交互操作。全局选项放在子命令前：

```sh
pipemixer --help
pipemixer --json list nodes
pipemixer --timeout 10000 set-volume YOUR_NODE 55 FL
pipemixer set-mute YOUR_NODE toggle
pipemixer get-default sink
pipemixer get-default source
```

多数节点目标接受真实 `node.name`、`id:N`，节点还可使用 `serial:N`；设备使用
`device.name` 或 ID。端口连接使用 `node.name:port.name` 或端口 `id:N`。
监听定义等持久策略有更严格的稳定名称要求，见对应章节。

`list nodes/devices/ports/links/graph/buses/sends/effects` 查询音频对象，
`list-scenes`、`list-route-rules`、`list-monitors`、`list-history` 和
`automation-status` 查询各类定义与运行状态。查询默认输出文本，支持时可加
`--json` 供脚本读取；普通修改命令成功后通常不输出文本，并等待实际状态确认。

| 退出码 | 含义                                          |
| ------ | --------------------------------------------- |
| `0`  | 成功                                          |
| `1`  | PipeWire 错误或超时                           |
| `2`  | 参数不合法                                    |
| `3`  | 目标、目标设备、声道、Route 或 Profile 不可用 |

默认普通命令超时 5000 ms。CLI 默认不要求 INI，显式 `--config` 时会读取指定文件。
完整命令签名见 `pipemixer --help` 和[英文总体说明](README.md)。

## 配置、文件与状态生命周期

配置根目录为 `$XDG_CONFIG_HOME/pipemixer`，未设置 XDG 时为 `~/.config/pipemixer`。
root 板级服务使用 `/root/.config/pipemixer`。

| 相对配置根目录的路径   | 内容                                     |
| ---------------------- | ---------------------------------------- |
| `pipemixer.ini`      | 界面、音量步长、鼠标、默认标签页与快捷键 |
| `scenes/NAME.json`   | 场景快照                                 |
| `startup-scene`      | 所选开机场景或`off`                    |
| `rules/NAME.json`    | 自动路由规则                             |
| `monitors/NAME.json` | 监听定义及正常混音/Solo/监听选择         |
| `automation.json`    | 自动化规则及外部控制配置                 |
| `automation-enabled` | 自动化的持久启停偏好                     |

INI 示例见 [assets/pipemixer.ini](assets/pipemixer.ini)。可以校验指定配置：

```sh
pipemixer --config /root/.config/pipemixer/pipemixer.ini --validate
```

场景、路由、监听和自动化配置使用原子写入，文件权限 0600。
每个配置目录具有独立作用域；正常使用时让 TUI、CLI 和板级服务使用同一配置目录。

| 状态                   | 退出 TUI 后        | 音频会话重启或整板重启后         |
| ---------------------- | ------------------ | -------------------------------- |
| 已创建总线、发送、效果 | 继续运行           | 由选定开机场景重建               |
| 自动路由定义           | 后台维护连接       | 服务重新读取并维护               |
| 监听定义与模式         | 继续运行           | 后台恢复；场景参数由所选场景恢复 |
| 自动化定义与启停偏好   | 保留，启用任务继续 | 监督服务重新加载，计数重新开始   |
| 正在进行的渐变/动作    | 继续运行           | 不恢复中途进度                   |
| 滚动缓存/正在录音      | 继续运行           | 不自动重新开始                   |
| 已完成录音文件         | 保留               | 持久存储上的文件保留             |

交付时测试夹具已清理，开机场景恢复为原来的 `off`，未留测试自动化规则。
需要自动恢复自己的布局时，先保存场景，再选择为开机场景。

## 编译、部署与板级服务

### 本机编译

需要 C 工具链、Meson/Ninja，以及 PipeWire、ncursesw、inih 开发依赖。在项目目录执行：

```sh
meson setup build
meson compile -C build
meson install -C build
```

安装需要对目标目录有写权限。Meson 同时构建主程序和 `pipemixer-dynamics.so`，
压缩器与限幅器需要这份配套插件。

### RK3506 / Luckfox 交叉编译

项目的 `build.sh` 使用 Luckfox Buildroot SDK，默认 SDK 为 `/home/kinho/Lyra-sdk`，
目标输出为 `rockchip_rk3506_luckfox`。SDK 需提供对应工具链和已暂存的依赖。

```sh
./build.sh
# SDK 在其他目录时：
SDK_ROOT=/path/to/Lyra-sdk ./build.sh
```

默认产物位于 `.cache/build-rk3506/build/pipemixer` 和同目录的
`pipemixer-dynamics.so`。

### 测试上传与持久安装

`./deploy.sh` 将程序和插件上传到板子 `/tmp/board/`，用于临时测试。
它不会更新持久安装，也不会安装开机服务；`/tmp` 内容会在重启后消失。

新安装或更新板级服务时，先在电脑上传程序及配套目录，保持原目录结构：

```sh
./deploy.sh
scp -r scripts assets docs root@192.168.123.100:/tmp/board/
```

然后在板子上执行安装脚本：

```sh
sh /tmp/board/scripts/install-board-startup /tmp/board/pipemixer
```

安装器默认查找二进制同目录的 DSP 插件；在其他位置时，可把插件路径作为第二个参数。
当前板子已安装完整版本，日常使用直接运行 `pipemixer`。

| 安装位置                                               | 内容                                                |
| ------------------------------------------------------ | --------------------------------------------------- |
| `/usr/local/bin/pipemixer`                           | 主程序                                              |
| `/usr/bin/pipemixer`                                 | 日常启动入口，旧入口保存为`.before-local-install` |
| `/usr/local/lib/pipemixer/pipemixer-dynamics.so`     | 动态处理插件                                        |
| `/usr/local/libexec/`                                | 音频/自动化监督脚本                                 |
| `/etc/init.d/S98pipemixer`                           | 音频会话、场景恢复、路由和监听服务                  |
| `/etc/init.d/S99pipemixer-automation`                | 自动化监督服务                                      |
| `/usr/local/share/pipemixer/automation/example.json` | 自动化示例                                          |
| `/usr/local/share/doc/pipemixer/AUTOMATION.md`       | 中文自动化说明                                      |

查看或管理服务：

```sh
/etc/init.d/S98pipemixer status
/etc/init.d/S99pipemixer-automation status
```

服务还支持 `start`、`stop` 和 `restart`。S98 停止时会关闭由它启动的音频服务，
会影响当前音频会话。音频/路由日志包括 `/tmp/pipemixer-session.log`、
`/tmp/pipemixer-pipewire.log`、`/tmp/pipemixer-wireplumber.log` 和
`/tmp/pipemixer-routing.log`；受管路径日志位于
`$XDG_RUNTIME_DIR/pipemixer-KIND-NAME.log`。

上述 root SysV 服务面向当前 Luckfox 板子。其他系统应接入已有用户音频服务，
在 PipeWire/会话管理器启动后执行 `restore-startup`，并监督路由/自动化引擎。
本板还安装了元数据通知兼容修复，适用版本及重建方法见[自动化说明](docs/AUTOMATION.md)。

## 验证范围与后续扩展

各阶段验证覆盖 TUI 操作、真实 PipeWire 图、实际音频 PCM、效果处理、场景/重连恢复、
服务异常恢复，以及真实整板重启。第六项最终完成 15 套板级回归，并额外验证了
电脑到板子的局域网 OSC。详细项目、资源数据和日志位置见[板级验证记录](tests/BOARD_VALIDATION.md)，
测试运行方法见[测试说明](tests/README.md)。

当前尚未实现的扩展：

- 内置多轨回放、更多录音格式与多声道布局。
- LUFS、过采样真峰值、FFT 频谱分析，以及前瞻/真峰值限幅。
- TUI 条件树与 MIDI/OSC 映射编辑器。
- 逐采样点自动化包络、任务中途断点恢复、独立硬件共享字时钟。
- MIDI 2.0、ALSA Sequencer、SysEx 参数协议、未来 OSC 时标调度与地址通配符。

尚未覆盖的实物或长时间测试：

- 实体 MIDI 控制器；现有 MIDI 测试使用 PTY 字符设备。
- 物理 USB 拔插；现有重连验证使用虚拟端点及真实 ALSA 设备重新枚举。
- 多实体硬件接口之间的 Route 切换；本板每个方向只有一个 analog Route。
- SD/eMMC/USB 长时间持续录音及所有处理/录音/控制同时运行的资源上限。

这些边界与已完成的阶段功能分别记录，后续可以按实际使用场景继续扩展。
