#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""摘除置顶窗口（WS_EX_TOPMOST）。

用法：
    python unpin_topmost.py              # 摘掉所有置顶窗口（保留系统 shell 窗口）
    python unpin_topmost.py Code.exe     # 只摘指定进程的置顶窗口
    python unpin_topmost.py --list       # 只列出当前置顶窗口，不修改

说明：
- 系统任务栏(Shell_TrayWnd)等 explorer 窗口默认跳过，避免破坏桌面正常行为。
- 常见场景：QQ 等置顶程序在前台时启动新程序，新窗口被 Windows 继承进置顶层。
"""
import ctypes
import subprocess
import sys
from ctypes import wintypes

u32 = ctypes.windll.user32
HWND_NOTOPMOST = -2
SWP_NOMOVE, SWP_NOSIZE, SWP_NOACTIVATE = 0x0002, 0x0001, 0x0010
WS_EX_TOPMOST = 0x00000008

# 跳过的系统 shell 窗口（进程名 -> 类名白名单）
SHELL_SKIP = {
    'explorer.exe': {'Shell_TrayWnd', 'ThumbnailDeviceHelperWnd', 'Progman', 'WorkerW'},
}

EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def proc_name(pid):
    out = subprocess.run(['tasklist', '/FI', f'PID eq {pid}', '/FO', 'CSV'],
                         capture_output=True).stdout.decode('gbk', 'replace')
    lines = [x for x in out.splitlines()[1:] if x.strip()]
    return lines[0].split('","')[0].strip('"') if lines else ''


def main():
    args = [a.lower() for a in sys.argv[1:]]
    list_only = '--list' in args
    targets = [a for a in args if not a.startswith('-')]

    topmost, unpinned = [], []
    seen = set()

    def cb(h, l):
        if not u32.IsWindowVisible(h):
            return True
        if u32.GetWindowLongW(h, -20) & WS_EX_TOPMOST:
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            name = proc_name(pid.value)
            cls = ctypes.create_unicode_buffer(128)
            u32.GetClassNameW(h, cls, 128)
            title = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, title, 256)
            key = (h, name, cls.value)
            if key in seen:
                return True
            seen.add(key)
            is_shell = cls.value in SHELL_SKIP.get(name.lower(), set())
            topmost.append((h, name, cls.value, title.value[:50], is_shell))
        return True

    u32.EnumWindows(EnumWindowsProc(cb), 0)

    for h, name, cls, title, is_shell in topmost:
        if is_shell:
            print(f'[跳过系统窗口] {name} class={cls}')
            continue
        if targets and name.lower() not in targets:
            continue
        if list_only:
            print(f'[置顶中] {name} pid窗口=0x{h:X} class={cls} title={title}')
        else:
            u32.SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            unpinned.append(f'{name} class={cls} title={title}')
            print(f'[已取消置顶] {name} class={cls} title={title}')

    if not list_only:
        print(f'\n共取消 {len(unpinned)} 个窗口的置顶。')


if __name__ == '__main__':
    main()
