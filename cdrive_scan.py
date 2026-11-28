# -*- coding: utf-8 -*-
"""只读扫描 C 盘占用：顶层目录 + 大目录二级明细 + 根目录大文件。不修改任何文件。"""
import os
import sys
import time
import ctypes
from pathlib import Path

ROOT = "C:\\"
REPORT = r"E:\Workdaddy\2026-11-20-17-31-00\C盘占用报告.txt"
FILE_ATTRIBUTE_REPARSE_POINT = 0x400
GetFileAttributesW = ctypes.windll.kernel32.GetFileAttributesW
GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
GetFileAttributesW.restype = ctypes.c_uint32


def is_reparse(p):
    a = GetFileAttributesW(p)
    return a != 0xFFFFFFFF and bool(a & FILE_ATTRIBUTE_REPARSE_POINT)


def dir_size(path):
    """递归统计目录大小（不跟随符号链接/联接点）"""
    total, files, errors = 0, 0, 0
    stack = [path]
    while stack:
        cur = stack.pop()
        try:
            with os.scandir(cur) as it:
                for e in it:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            if not is_reparse(e.path):
                                stack.append(e.path)
                        elif e.is_file(follow_symlinks=False):
                            total += e.stat(follow_symlinks=False).st_size
                            files += 1
                    except OSError:
                        errors += 1
        except OSError:
            errors += 1
    return total, files, errors


def fmt(n):
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if n < 1024 or unit == 'TB':
            return f'{n:.2f} {unit}'
        n /= 1024


def main():
    t0 = time.time()
    lines = []
    lines.append('C 盘占用只读扫描报告  ' + time.strftime('%Y-%m-%d %H:%M:%S'))
    lines.append('说明：仅统计大小，未移动/删除任何文件；跳过符号链接和联接点；无权访问的条目计入错误数。')
    lines.append('')

    top = []
    root_files = []
    with os.scandir(ROOT) as it:
        for e in it:
            try:
                if e.is_dir(follow_symlinks=False):
                    if is_reparse(e.path):
                        continue
                    size, files, errors = dir_size(e.path)
                    top.append((size, e.path, files, errors))
                    print(f'  扫描 {e.name}: {fmt(size)} ({files} 个文件)', flush=True)
                elif e.is_file(follow_symlinks=False):
                    sz = e.stat(follow_symlinks=False).st_size
                    if sz > 512 * 1024 * 1024:
                        root_files.append((sz, e.path))
            except OSError:
                pass

    top.sort(reverse=True)
    total_all = sum(x[0] for x in top)
    lines.append(f'== C 盘顶层目录占用（合计 {fmt(total_all)}）==')
    for i, (size, path, files, errors) in enumerate(top[:20], 1):
        lines.append(f'{i:2d}. {path:45s} {fmt(size):>12s}  {files:8d} 文件' +
                     (f'  跳过/拒绝 {errors}' if errors else ''))
    lines.append('')

    if root_files:
        lines.append('== 根目录大文件（>512 MB）==')
        for sz, path in sorted(root_files, reverse=True):
            lines.append(f'   {fmt(sz):>12s}  {path}')
        lines.append('')

    lines.append('== 占用最高 6 个目录的二级明细 ==')
    for size, path, files, errors in top[:6]:
        sub = []
        try:
            with os.scandir(path) as it2:
                for e in it2:
                    try:
                        if e.is_dir(follow_symlinks=False) and not is_reparse(e.path):
                            s, f, er = dir_size(e.path)
                            sub.append((s, e.path, f))
                        elif e.is_file(follow_symlinks=False):
                            sz = e.stat(follow_symlinks=False).st_size
                            if sz > 100 * 1024 * 1024:
                                sub.append((sz, e.path, 1))
                    except OSError:
                        pass
        except OSError:
            pass
        sub.sort(reverse=True)
        lines.append(f'-- {path} ({fmt(size)})')
        for s, p, f in sub[:12]:
            lines.append(f'     {fmt(s):>12s}  {p}')
        lines.append('')

    lines.append(f'扫描耗时 {time.time()-t0:.1f} 秒。')
    out = '\n'.join(lines)
    Path(REPORT).write_text(out, encoding='utf-8')
    print('\n' + out, flush=True)


if __name__ == '__main__':
    main()
