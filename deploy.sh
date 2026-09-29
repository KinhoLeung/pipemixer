#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${PIPEMIXER_BUILD_ROOT:-$PROJECT_DIR/.cache/build-rk3506}"
LOCAL_BINARY="$BUILD_ROOT/build/pipemixer"
REMOTE_NAME="pipemixer"
BOARD_HOST="${BOARD_HOST:-192.168.123.100}"
BOARD_USER="${BOARD_USER:-root}"
# 可通过 BOARD_PASSWORD 环境变量覆盖这个默认密码。
BOARD_PASSWORD="${BOARD_PASSWORD:-luckfox}"

if [[ ! -x "$LOCAL_BINARY" ]]; then
    echo "PipeMixer binary not found: $LOCAL_BINARY" >&2
    echo "Run ./build.sh first." >&2
    exit 1
fi
command -v sftp >/dev/null || { echo "sftp is required" >&2; exit 1; }
command -v expect >/dev/null || { echo "expect is required for password-based SFTP" >&2; exit 1; }

export BOARD_HOST BOARD_USER BOARD_PASSWORD LOCAL_BINARY REMOTE_NAME
expect <<'EXPECT_EOF'
set timeout 180
spawn sftp -oBatchMode=no -oPreferredAuthentications=password,keyboard-interactive -oPubkeyAuthentication=no -oStrictHostKeyChecking=accept-new $env(BOARD_USER)@$env(BOARD_HOST)
expect {
    -re {(?i)password:} {
        send -- "$env(BOARD_PASSWORD)\r"
        exp_continue
    }
    -re {(?i)permission denied} {
        puts stderr "SFTP authentication failed"
        exit 1
    }
    -re {sftp> ?$} {}
    timeout {
        puts stderr "Timed out while connecting to SFTP"
        exit 1
    }
    eof {
        puts stderr "SFTP disconnected before login completed"
        exit 1
    }
}

# mkdir may report that the directory already exists; the following put will
# report a hard error if the destination is otherwise unavailable.
send -- "mkdir /tmp/board\r"
expect {
    -re {sftp> ?$} {}
    timeout { puts stderr "Timed out creating /tmp/board"; exit 1 }
    eof { puts stderr "SFTP disconnected while creating /tmp/board"; exit 1 }
}

send -- "put -p \"$env(LOCAL_BINARY)\" \"/tmp/board/$env(REMOTE_NAME)\"\r"
expect {
    -re {(?i)(couldn't|failure|no such file|permission denied)} {
        puts stderr "SFTP upload failed"
        exit 1
    }
    -re {sftp> ?$} {}
    timeout { puts stderr "Timed out uploading $env(REMOTE_NAME)"; exit 1 }
    eof { puts stderr "SFTP disconnected during upload"; exit 1 }
}

send -- "chmod 755 \"/tmp/board/$env(REMOTE_NAME)\"\r"
expect {
    -re {(?i)(couldn't|failure|no such file|permission denied)} {
        puts stderr "Could not set executable permissions on the uploaded binary"
        exit 1
    }
    -re {sftp> ?$} {}
    timeout { puts stderr "Timed out setting remote permissions"; exit 1 }
    eof { puts stderr "SFTP disconnected while setting permissions"; exit 1 }
}

send -- "bye\r"
expect eof
set result [wait]
set status [lindex $result 3]
if {$status != 0} {
    puts stderr "sftp exited with status $status"
    exit $status
}
EXPECT_EOF

echo "Uploaded to $BOARD_USER@$BOARD_HOST:/tmp/board/$REMOTE_NAME"

# 上传的是程序本身；板子的系统镜像仍需提供 PipeWire、ncursesw 和 inih 运行库。
# PipeMixer 可调音量/静音、默认设备和设备 Profile/Route，但不能创建节点间连线。
# 电脑 UAC 音频转到 UDA1334 时，需另开 qpwgraph 将 UAC2Gadget 的
# capture_FL/FR 分别连接到 rockchip,gcodec 的 playback_FL/FR。
# 若板子重启后 PipeWire/WirePlumber 未运行，以 root SSH 登录板子，按顺序执行：
#   mkdir -p /run/user/0 && chmod 700 /run/user/0
#   export XDG_RUNTIME_DIR=/run/user/0
#   [ -S "$XDG_RUNTIME_DIR/pipewire-0" ] || nohup /usr/bin/pipewire >/tmp/pipewire.log 2>&1 </dev/null &
#   while [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; do sleep 1; done
#   ps w | grep -q '[w]ireplumber' || nohup /usr/bin/wireplumber >/tmp/wireplumber.log 2>&1 </dev/null &
#   XDG_RUNTIME_DIR=/run/user/0 /usr/bin/wpctl status
# PipeWire 应先于 WirePlumber 启动；/run 是临时目录，重启后需要重新启动这两个服务。
# 部署后在电脑终端停止旧的同名工具并通过 SSH TTY 启动 PipeMixer：
#   killall pipemixer 2>/dev/null || true
#   XDG_RUNTIME_DIR=/run/user/0 /tmp/board/pipemixer
# 在 TUI 会话中按 Ctrl+C 停止 PipeMixer；PipeWire 和 WirePlumber 保持运行。
