#!/usr/bin/env python3
"""
mkxnuimg-psyche.py —— 把 psyche 的 UEFI + loader payload 打包成 fastboot boot 用的 boot.img

**严格照抄 HakiOS 的 mkxnuimg.py 的做法**（我第一次写错了 DTB 的处理方式）：
  blob   = BootShim + UEFI_FD  [+ XNUP payload]
  kernel = gzip(blob) + DTB            ← DTB 拼在 gzip 后面，不是 --dtb 参数
  mkbootimg --kernel kernel --ramdisk ramdisk --header_version 1

psyche 与 redwood 的全部差别（真机实测）：
  FD_BASE  0x9FC00000   （redwood 0x9FD00000；psyche 的 uefiplat.cfg 明写）
  FD_SIZE  0x300000
  DTB      psyche 自己的 842,746 字节

部件：
  fd        /root/hakios/devices/psyche/prebuilt/SILICIUM_UEFI.fd
  dtb       /mnt/d/DSH/IOS/_psyche-assets/psyche.dtb
  shim      Mu-Silicium/BootShim/BootShim.bin（按 psyche 的 FD_BASE 现编）
  ramdisk   Mu-Silicium/Resources/ramdisk
"""

import argparse
import gzip
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
MU = os.path.join(ROOT, 'Mu-Silicium')

FD_BASE = 0x9FC00000
FD_SIZE = 0x300000
# payload 放在 Display 保留区（0x9C000000-0x9E400000, EfiReservedMemoryType）的尾部：
# 显示实际只用 9.89 MB，尾部 26 MB 空着；UEFI 不会分配保留区，所以这边的内容不会被清零。
# （原来的 0xF0000000 落在 XBL 报的大块可用内存里，被 UEFI 清零过，见 logdump 分析。）
PAYLOAD_BASE = 0xF0000000
PAYLOAD_MAX = 0x04000000

DTB = os.path.join(ROOT, 'devices', 'psyche', 'prebuilt', 'device-tree.dtb')  # psyche.dtb md5 9daef336
# 11-03/11-24 实机成功的镜像用的是 thyme FD（≡ redwood 的 SILICIUM_UEFI.fd，md5 2acd6396）；psyche FD 作诊断备选
FD = os.path.join(ROOT, 'devices', 'redwood', 'prebuilt', 'SILICIUM_UEFI.fd')

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload', help='XNUP payload.bin')
ap.add_argument('-o', '--output', default=os.path.join(ROOT, 'xnu-psyche.img'))
ap.add_argument('--fd', default=FD, help='UEFI FD（默认 psyche 自建的；诊断时可用 thyme 的）')
args = ap.parse_args()

FD = args.fd

shim_dir = os.path.join(ROOT, 'devices', 'psyche', 'prebuilt', 'BootShim-src')
for name, path in {
    'fd': FD,
    'dtb': DTB,
    'shim': os.path.join(shim_dir, 'BootShim.S'),
    'ramdisk': os.path.join(ROOT, 'devices', 'psyche', 'prebuilt', 'ramdisk'),
    'mkbootimg': os.path.join(ROOT, 'devices', 'psyche', 'prebuilt', 'mkbootimg.py'),
}.items():
    if not os.path.isfile(path):
        sys.exit(f'mkxnuimg-psyche: {name} 不存在: {path}')

# BootShim（psyche 的 FD_BASE）
for f in ('BootShim.elf', 'BootShim.bin'):
    try:
        os.unlink(os.path.join(shim_dir, f))
    except FileNotFoundError:
        pass
env = dict(os.environ)
env['CC'] = os.environ.get('CC', 'clang')
env['OBJCOPY'] = os.environ.get('OBJCOPY', 'llvm-objcopy')
print(f'BootShim: FD_BASE={FD_BASE:#x} FD_SIZE={FD_SIZE:#x}')
subprocess.check_call([
    'make', 'REQUIRES_KERNEL_HEADER=1',
    f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
    f'PAYLOAD_BASE={PAYLOAD_BASE:#x}',
    f'CFLAGS=--target=aarch64-linux-gnu',
], cwd=shim_dir, env=env)

shim = open(os.path.join(shim_dir, 'BootShim.bin'), 'rb').read()
fd = open(FD, 'rb').read()
assert len(fd) == FD_SIZE, f'FD 大小 {len(fd):#x} != {FD_SIZE:#x}'
print(f'  shim    {len(shim)} bytes')
print(f'  fd      {len(fd):#x} bytes')

blob = shim + fd
if args.payload:
    data = open(args.payload, 'rb').read()
    assert data[:4] == b'XNUP', 'bad payload (no XNUP magic)'
    assert len(data) <= PAYLOAD_MAX - 0x1000, f'payload 太大 {len(data)}'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'  payload {len(data)} bytes (bytesum={sum(data) & 0xffffffff:08x})')

# *** 关键：DTB 拼在 gzip 后面，不是 --dtb ***
dtb = open(DTB, 'rb').read()
kernel = args.output + '.kernel~'
open(kernel, 'wb').write(gzip.compress(blob, 6) + dtb)
try:
    subprocess.check_call([
        sys.executable, os.path.join(ROOT, 'devices', 'redwood', 'prebuilt', 'mkbootimg.py'),
        '--kernel', kernel,
        '--ramdisk', os.path.join(ROOT, 'devices', 'redwood', 'prebuilt', 'ramdisk'),
        '--header_version', '1',
        '-o', args.output,
    ])
finally:
    os.unlink(kernel)

print(f'\n{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
