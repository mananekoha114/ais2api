#!/bin/sh
# Copyright (c) 2026 Mana Nekoha (mananekoha114)
# SPDX-License-Identifier: MPL-2.0
# One-click launcher at the root of the Linux and Termux runtime archives.
# It initializes config.json and data/ beside the executable on first use,
# offers to run setup when no browser is available, and then runs the service
# in this terminal until Ctrl+C. --background starts it detached instead.
# Every other argument (for example --port 18452) is passed to each command.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
program="$root/bin/ais2api"
if [ ! -x "$program" ] && [ -f "$program.exe" ]; then
  program="$program.exe" # Git Bash or MSYS on Windows
fi
if [ ! -x "$program" ]; then
  echo "找不到可执行文件：$program" >&2
  exit 1
fi
background=0
if [ "${1:-}" = --background ]; then
  background=1
  shift
fi

# Captured stdout is single-line JSON; errors still reach the terminal readably.
# Prints a string or number field; JSON-escaped backslashes are unescaped.
field() {
  printf '%s\n' "$1" |
    sed -n -e "s/.*\"$2\":\"\([^\"]*\)\".*/\1/p" -e t -e "s/.*\"$2\":\([0-9]*\).*/\1/p" |
    sed 's/\\\\/\\/g'
}

status=$("$program" status "$@")
case $status in
  *'"running":true'*)
    echo "ais2api 已在运行。查看状态：\"$program\" status  停止服务：\"$program\" stop"
    exit 0 ;;
esac

init=$("$program" init "$@")
case $init in
  *'"api_key_configured":false'*)
    echo "提示：$(field "$init" config_file) 中的 api_key 为空；调用生成接口前，请先填写它或在管理控制台「配置」中设置。" ;;
esac
case $init in
  *'"admin_password_default":true'*)
    echo "提示：管理控制台的默认密码为 admin，首次登录后需要修改。" ;;
esac

doctor=$("$program" doctor "$@")
case $doctor in
  *'"browser_exists":true'*) ;;
  *)
    echo "浏览器尚未准备或需要更新。setup 会下载浏览器，并安装所需的系统依赖（Linux 可能通过 sudo 使用 apt-get，Termux 会创建 proot Ubuntu 容器）。"
    if [ ! -t 0 ]; then
      echo "请在终端中确认后运行：\"$program\" setup --yes" >&2
      exit 1
    fi
    printf '是否现在运行 setup？[y/N] '
    read -r answer || answer=
    case $answer in
      [yY] | [yY][eE][sS]) ;;
      *) echo "已取消。"; exit 1 ;;
    esac
    "$program" setup --yes "$@" >/dev/null
    echo "浏览器已准备完成。"
    ;;
esac

if [ "$background" = 1 ]; then
  "$program" start "$@" >/dev/null
  settings=$("$program" config show "$@")
  host=$(field "$settings" bind)
  case $host in
    0.0.0.0 | '') host=127.0.0.1 ;;
    ::) host='[::1]' ;;
    *:*) host="[$host]" ;;
  esac
  echo "ais2api 已在后台启动：http://$host:$(field "$settings" port)/"
  echo "日志：$(field "$doctor" service_log)  停止服务：\"$program\" stop"
  exit 0
fi
echo "正在启动 ais2api，按 Ctrl+C 停止。"
exec "$program" serve "$@"
