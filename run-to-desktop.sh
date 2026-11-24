#!/bin/bash
# run-to-desktop.sh —— 从 fastboot 一路跑到 iOS 桌面（本包实测流程的自动化版）
#
#   bash run-to-desktop.sh              # 全流程
#   bash run-to-desktop.sh --no-ramdisk # system.apfs 已在手机内存里，跳过 2.3 GB 上传
#
# 不刷任何分区；全程只在手机内存里跑。失败先看 docs/boot-to-desktop.md 第 8 节。
set -u
cd "$(dirname "$0")" || exit 1
FB=${FASTBOOT:-fastboot}

say() { printf '\n=== %s\n' "$*"; }
die() { printf '!! %s\n' "$*" >&2; exit 1; }

command -v "$FB" >/dev/null || die "找不到 fastboot（装 platform-tools 并加进 PATH）"
mkdir -p logs

say "0/5 关掉 adb（它占 5037 会让 fastboot 连接失败）"
adb kill-server 2>/dev/null || true
sleep 1

say "1/5 手机状态"
if ! "$FB" devices 2>/dev/null | grep -q fastboot; then
	die "手机不在 fastboot：先 adb reboot bootloader，或手动进 bootloader"
fi
"$FB" devices

if ! "$FB" getvar product 2>&1 | grep -q 'product: xnuloader'; then
	say "   还不在 loader：fastboot boot xnu.img"
	"$FB" boot xnu.img || die "loader 起不来"
	sleep 8
	"$FB" getvar product 2>&1 | grep -q 'product: xnuloader' || die "loader 没应答（product 不是 xnuloader）"
fi

if [ "${1:-}" != "--no-ramdisk" ]; then
	say "2/5 上传常驻根镜像 userland/system.apfs（2.3 GB，2-10 分钟）"
	"$FB" oem ramdisk || die "loader 拒绝预留内存窗口"
	FB_TIMEOUT=1800 "$FB" stage userland/system.apfs || die "上传失败：先 fastboot oem ramdisk none 再重试"
else
	say "2/5 跳过 system.apfs 上传（--no-ramdisk）"
fi

say "3/5 上传 payload（内核 + root.apfs）"
FB_TIMEOUT=120 "$FB" stage run-payload.bin || die "payload 上传失败"

say "4/5 跑内核"
"$FB" oem run || die "oem run 被拒"

say "5/5 等它跑完（默认 240 秒自返），取日志"
for i in $(seq 1 130); do
	r=$("$FB" getvar result 2>&1 | sed -n 's/^result: //p')
	case "$r" in
		running*) sleep 2 ;;
		"") sleep 2 ;;
		*) printf '结果: %s\n' "$r"; break ;;
	esac
done

"$FB" oem log >/dev/null 2>&1 && "$FB" get_staged logs/boot.log >/dev/null 2>&1 \
	&& echo "日志: logs/boot.log"

if [ -s logs/boot.log ]; then
	say "成功判定"
	grep -E 'HakiosGPU: .*submissions|MiniIOMobileFramebuffer: .*frames|psyche_touch: .*touches|battery: SOC|StartWindowServer: initial main screen' logs/boot.log | tail -8 || true
fi

printf '\n回 Android: fastboot reboot\n'
