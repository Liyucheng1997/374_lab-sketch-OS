# SketchOS · 手绘操作系统

基于原版 Linux 6.12 内核与自编用户态的教学操作系统，提供手绘风格桌面，可通过浏览器中的 v86 模拟器或 QEMU 运行。

当前仓库发布版本：**v1.1.0**（新增“音乐”播放器和 SB16 声卡支持，网页版和 QEMU 版都能放出声音）。

## 快速体验

在线体验（浏览器里直接启动）：<https://liyucheng1997.github.io/374_lab-sketch-OS/>
——每次推送到 main，GitHub Actions 会完整构建一遍并发布网页版（见 `.github/workflows/pages.yml`）。

从 GitHub Releases 下载 `SketchOS-v1.1.0-web.zip` 并解压，在安装了 Python 或 Node.js 的 Windows 上双击 `windows/启动网页版.bat`。

也可在解压目录运行：

```sh
python tools/serve.py --open
```

通过本地 HTTP 服务打开网页，不要直接双击 `web/index.html`。

## AI 功能：一台由 AI 驱动的电脑

SketchOS 内置了 AI 服务 `skaid`，接入 Claude（默认模型 **Claude Opus 5.5**，`claude-opus-5-5`）。AI 不只是聊天——
它能在这台电脑上**运行命令、读写和修改文件、列目录、打开图形应用**，把一件事从头做到尾，每一步都显示给你看。

1. 打开网页版，点工具栏上的「AI 设置」，填入 Anthropic API 密钥（在 <https://console.anthropic.com> 创建）。
   可以换模型、调整思考深度（effort），也可以填兼容的 API 地址。点「测试连接」检查密钥是否可用。
2. 在 SketchOS 里使用：
   - 双击桌面上的「AI助手」，或者按 <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+<kbd>A</kbd>，在窗口里告诉它你想做什么；
   - 在终端里输入 `ai 帮我在文档里写一首诗并用记事本打开`，`ps | ai 哪个进程占内存最多`，或者只输入 `ai` 进入多轮对话。

虚拟机没有网卡：网页给它插了第二个串口（`/dev/ttyS1`），skaid 把请求写进串口，由网页加上密钥发给 Claude，再把流式回应写回来。
对话和工具调用全在虚拟机里完成；**API 密钥只保存在你的浏览器里**（localStorage），只发给你填的 API 地址，虚拟机里的程序看不到它。
QEMU 版没有这座“桥”，AI 功能只在网页版里可用。细节见 [开发规范](docs/开发规范.md) 4.7 节。

## 从源码构建

在 WSL Ubuntu 中进入项目目录：

```sh
bash scripts/build.sh
```

构建脚本会准备工具链、编译内核与用户态、打包根文件系统并准备网页资源。首次构建需要联网，安装系统依赖可能需要 sudo。

构建完成后，在 Windows 双击 `windows/启动网页版.bat`，或在 WSL 运行：

```sh
bash scripts/run-qemu.sh
```

`out/`、`web/lib/` 和 `web/images/` 为生成或下载的运行资源，不纳入源码提交；预构建网页版随 Release 单独提供，
在线版本由 GitHub Actions 构建后发布到 GitHub Pages。

## 项目结构

- `kernel/`：Linux 内核配置。
- `userspace/`：初始化程序、Shell、命令工具箱、AI 服务、图形库、显示服务器与桌面应用。
- `rootfs/`：根文件系统配置和初始文件。
- `web/`：浏览器入口、界面和模拟器集成。
- `scripts/`、`tools/`、`windows/`：构建、测试与启动工具。
- `tests/`：现有测试。
- `assets/fonts/`：字体资源及相关许可证。

开发与测试说明见 [开发规范](docs/开发规范.md)。第三方组件及字体请遵守各自附带的许可证。
