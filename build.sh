#!/usr/bin/env bash
# ============================================================
# NewRpa-Next 一键编译脚本  (Git Bash / MSYS2)
#
#   ./build.sh                       增量编译（Debug，4 线程）
#   ./build.sh -j 8                  8 线程
#   ./build.sh -t bcb_plugin         只编译北京银行插件 DLL（迭代最快）
#   ./build.sh --release             Release 构建（自动重配 + 全量重编）
#   ./build.sh -c                    --clean：清空 build-mingw 全新构建
#   ./build.sh --kill-chrome         编译前同时结束 chrome.exe
#   ./build.sh --run                 编译成功后直接运行 newrpa.exe
#   ./build.sh -- run --config config.json --bank BOBQ --action all --chrome-port 9222
#                                   自定义运行参数（隐含 --run）
#
#   环境变量可覆盖：MINGW_BIN / CMAKE_BIN / JOBS / BUILD_TYPE / BUILD_DIR
# ============================================================
set -Eeuo pipefail

# ---------------- 工具链（换机器只改这两行） ----------------
MINGW_BIN="${MINGW_BIN:-/e/Workdaddy/mingw64/bin}"          # g++ / mingw32-make
CMAKE_BIN="${CMAKE_BIN:-/e/Workdaddy/mingw32/mingw32/bin}"  # cmake / ninja
export PATH="$MINGW_BIN:$CMAKE_BIN:$PATH"

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-mingw}"
BIN_DIR="$BUILD_DIR/bin"
JOBS="${JOBS:-4}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"

CLEAN=0
KILL_CHROME=0
DO_RUN=0
TARGET=""
RUN_ARGS=()

die() { echo "[ERROR] $*" >&2; exit 1; }
trap 'echo "[FAIL] 编译脚本异常终止，请查看上方错误输出" >&2' ERR

usage() {
  cat <<'EOF'
用法: ./build.sh [选项]

  -h, --help              显示本帮助
  -c, --clean             删除 build-mingw 后全新构建（不可逆，谨慎使用）
  -j N                    并行线程数（默认 4，等价环境变量 JOBS）
  -t, --target NAME       只编译指定目标，如 bcb_plugin / newrpa / NewRpaCore
      --release           Release 构建（默认 Debug）
      --debug             强制 Debug 构建
      --kill-chrome       编译前同时结束 chrome.exe（默认只结束 newrpa.exe）
      --run               编译成功后自动运行 newrpa.exe（默认 login 流程）
  -- <args...>            自定义 newrpa.exe 参数，隐含 --run

示例:
  ./build.sh
  ./build.sh -j 8 -t bcb_plugin
  ./build.sh --clean --release
  ./build.sh -- run --config config.json --bank BOBQ --action all --chrome-port 9222

环境变量: MINGW_BIN, CMAKE_BIN, JOBS, BUILD_TYPE, BUILD_DIR
EOF
}

# ---------------- 参数解析 ----------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)      usage; exit 0 ;;
    -c|--clean)     CLEAN=1; shift ;;
    -j)             [[ $# -ge 2 ]] || die "-j 缺少线程数"; JOBS="$2"; shift 2 ;;
    -j*)            JOBS="${1#-j}"; shift ;;
    -t|--target)    [[ $# -ge 2 ]] || die "--target 缺少目标名"; TARGET="$2"; shift 2 ;;
    --release)      BUILD_TYPE=Release; shift ;;
    --debug)        BUILD_TYPE=Debug; shift ;;
    --kill-chrome)  KILL_CHROME=1; shift ;;
    --run)          DO_RUN=1; shift ;;
    --)             shift; RUN_ARGS=("$@"); DO_RUN=1; break ;;
    *)              die "未知参数: $1（用 -h 查看用法）" ;;
  esac
done
[[ "$JOBS" =~ ^[0-9]+$ ]] || die "线程数必须是数字: $JOBS"

echo "============================================================"
echo " NewRpa-Next 编译"
echo " 源码目录 : $ROOT"
echo " 构建目录 : $BUILD_DIR"
echo " 构建类型 : $BUILD_TYPE   并行: $JOBS   目标: ${TARGET:-ALL}"
echo "============================================================"

# ---------------- 1/5 环境自检 ----------------
for tool in g++ cmake mingw32-make; do
  command -v "$tool" >/dev/null 2>&1 || die "找不到 $tool —— 请检查 MINGW_BIN / CMAKE_BIN"
done
echo "[1/5] 工具链 OK"
echo "      g++   : $(g++ --version | head -1)"
echo "      cmake : $(cmake --version | head -1)"
echo "      make  : $(mingw32-make --version | head -1)"

# ---------------- 2/5 结束占用 DLL 的进程 ----------------
# newrpa.exe 会加载 libNewRpaCore.dll / plugins/*.dll，不结束会导致链接报
# "cannot open output file ... Permission denied"。
echo "[2/5] 结束残留进程（避免 DLL 被占用导致链接失败）"
taskkill //F //IM newrpa.exe >/dev/null 2>&1 && echo "      newrpa.exe 已结束" || echo "      newrpa.exe 未运行"
if [[ "$KILL_CHROME" -eq 1 ]]; then
  taskkill //F //IM chrome.exe >/dev/null 2>&1 && echo "      chrome.exe 已结束" || echo "      chrome.exe 未运行"
fi

# ---------------- 3/5 清理（仅显式 -c） ----------------
if [[ "$CLEAN" -eq 1 && -d "$BUILD_DIR" ]]; then
  echo "[3/5] --clean：删除 $BUILD_DIR"
  rm -rf -- "$BUILD_DIR"
else
  echo "[3/5] 跳过清理（增量构建；需要全新构建请加 --clean）"
fi

# Windows 原生 cmake 用混合路径 E:/... 最稳，shell 自身操作仍用 POSIX 路径
if command -v cygpath >/dev/null 2>&1; then
  ROOT_CFG="$(cygpath -m "$ROOT")"
  BUILD_CFG="$(cygpath -m "$BUILD_DIR")"
else
  ROOT_CFG="$ROOT"; BUILD_CFG="$BUILD_DIR"
fi

# ---------------- 4/5 configure（首次 / 构建类型变化 / clean 后） ----------------
NEED_CFG=0
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  NEED_CFG=1
elif ! grep -qx "CMAKE_BUILD_TYPE:STRING=$BUILD_TYPE" "$BUILD_DIR/CMakeCache.txt"; then
  NEED_CFG=1
fi
if [[ "$NEED_CFG" -eq 1 ]]; then
  echo "[4/5] CMake configure (-G 'MinGW Makefiles', $BUILD_TYPE) ..."
  cmake -S "$ROOT_CFG" -B "$BUILD_CFG" -G "MinGW Makefiles" \
        -DCMAKE_CXX_COMPILER=g++ \
        -DCMAKE_MAKE_PROGRAM=mingw32-make \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || die "CMake configure 失败"
else
  echo "[4/5] CMakeCache 已存在且构建类型一致，跳过 configure"
fi

# ---------------- 5/5 build ----------------
BUILD_ARGS=(--parallel "$JOBS")
if [[ -n "$TARGET" ]]; then
  BUILD_ARGS+=(--target "$TARGET")
fi
echo "[5/5] 编译中：cmake --build $BUILD_CFG ${BUILD_ARGS[*]}"
cmake --build "$BUILD_CFG" "${BUILD_ARGS[@]}" || die "编译失败"

# ---------------- 产物清单 ----------------
echo
echo "==================== 编译成功 ===================="
for f in "$BIN_DIR/newrpa.exe" \
         "$BIN_DIR/plugins/libbcb.dll" "$BIN_DIR/plugins/libicbc.dll" \
         "$BIN_DIR/plugins/libklb.dll"; do
  if [[ -f "$f" ]]; then
    printf '  %-24s %10s B  %s\n' \
      "$(basename "$f")" "$(stat -c %s "$f")" "$(stat -c %y "$f" | cut -d. -f1)"
  fi
done
echo "  运行入口 : $BIN_DIR/newrpa.exe"
echo "  插件目录 : $BIN_DIR/plugins/   (newrpa.exe 实际加载这里的 DLL)"

# ---------------- 可选：编译后直接运行 ----------------
if [[ "$DO_RUN" -eq 1 ]]; then
  if (( ${#RUN_ARGS[@]} == 0 )); then
    RUN_ARGS=(run --config config.json --bank BOBQ --action login --chrome-port 9222)
  fi
  echo
  echo ">>> 运行: newrpa.exe ${RUN_ARGS[*]}"
  ( cd "$BIN_DIR" && ./newrpa.exe "${RUN_ARGS[@]}" )
fi
