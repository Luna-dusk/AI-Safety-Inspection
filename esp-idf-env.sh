# ESP-IDF 环境自动加载（供 VS Code 终端 profile 使用）
# 手动使用：bash --rcfile <本文件> -i

# --rcfile 会替代 ~/.bashrc，先恢复用户自己的配置
if [ -f "$HOME/.bashrc" ]; then
    . "$HOME/.bashrc"
fi

# 若 .bashrc 没有加载 ESP-IDF，则自动探测并加载
if [ -z "$IDF_PATH" ] || [ ! -f "$IDF_PATH/export.sh" ]; then
    for d in "$HOME/esp/esp-idf" "$HOME/esp-idf" "/opt/esp-idf" "/usr/local/esp-idf"; do
        if [ -f "$d/export.sh" ]; then
            IDF_PATH="$d"
            break
        fi
    done
fi

if [ -z "$IDF_PATH" ] || [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "[esp-idf-env] 未找到 ESP-IDF，请设置 IDF_PATH 后重试" >&2
    return 0 2>/dev/null || exit 0
fi

export IDF_PATH
# 工具链默认在 ~/.espressif；自定义过就改这里
# export IDF_TOOLS_PATH="$HOME/.espressif"

# 触发条件：idf.py 不可用，或 IDF venv 尚未激活。
# 不能只看 idf.py 是否在 PATH 上——esp-idf 源码目录（tools/idf.py）常被
# .profile 加进 PATH，但那只是脚本文件，没有 venv 里的 esp_idf_monitor
# 模块，运行必然报 "No module named 'esp_idf_monitor'"。
if ! command -v idf.py >/dev/null 2>&1 || [ -z "$IDF_PYTHON_ENV_PATH" ]; then
    # 第一步（在 source export.sh 之前）：把 IDF venv 提到 PATH 最前。
    # conda/pyenv 的 python（如 miniconda 的 3.14）排在最前时，export.sh 会按它
    # 探测 python 版本去找 venv（如 idf5.4_py3.14_env），而实际安装的是 py3.10
    # 的 venv，于是报 "ESP-IDF Python virtual environment not found" 直接失败。
    if [ -z "$IDF_PYTHON_ENV_PATH" ]; then
        idf_ver_raw="$(cat "$IDF_PATH/version.txt" 2>/dev/null \
            || git -C "$IDF_PATH" describe --tags --always 2>/dev/null)"
        idf_ver_short="$(printf '%s' "$idf_ver_raw" \
            | sed -nE 's/^v?([0-9]+\.[0-9]+).*/\1/p')"
        tools_root="${IDF_TOOLS_PATH:-$HOME/.espressif}/python_env"
        # 优先选 IDF 版本一致的 venv，其次退回任意一个可用的
        for cand in "$tools_root"/idf"${idf_ver_short}"_py*_env \
                    "$tools_root"/idf*_py*_env; do
            if [ -x "$cand/bin/python" ]; then
                IDF_PYTHON_ENV_PATH="$cand"
                break
            fi
        done
    fi
    if [ -n "$IDF_PYTHON_ENV_PATH" ] && [ -d "$IDF_PYTHON_ENV_PATH/bin" ]; then
        export IDF_PYTHON_ENV_PATH
        export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
    fi
    if ! . "$IDF_PATH/export.sh" >/dev/null 2>&1; then
        echo "[esp-idf-env] 加载 $IDF_PATH/export.sh 失败，请手动执行: . $IDF_PATH/export.sh" >&2
        return 0 2>/dev/null || exit 0
    fi
fi

# 第二步（兜底）：.bashrc 里 conda/pyenv 前置的 python 若仍盖过 IDF venv，
# idf.py 会报 "No module named 'esp_idf_monitor'"，这里再确保 venv 在最前。
if [ -n "$IDF_PYTHON_ENV_PATH" ] && [ -d "$IDF_PYTHON_ENV_PATH/bin" ]; then
    case ":$PATH:" in
        "$IDF_PYTHON_ENV_PATH/bin:"*) ;;
        *) export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH" ;;
    esac
    hash -r 2>/dev/null || true
fi

idf_ver=$(cat "$IDF_PATH/version.txt" 2>/dev/null \
    || git -C "$IDF_PATH" describe --tags --always 2>/dev/null \
    || echo "unknown")
printf '[esp-idf-env] ESP-IDF %s 就绪 | python=%s\n' \
    "$idf_ver" "$(command -v python)"
