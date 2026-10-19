# quickshell-clipboard

一个 Qt 6 QML 模块（`io.yigekuyou.clipboard`），在 Wayland 下实现剪贴板管理器：
无焦点也能读取和替换 selection，并把文本/图片历史以**纯内存**方式提供给 QML。

它最初为 [Quickshell](https://quickshell.org/) 编写，但模块本身不依赖 Quickshell，
任何 Qt Quick 应用都可以 `import io.yigekuyou.clipboard` 使用。

许可证：**GPL-3.0-or-later**（与 Qt 的开源授权要求兼容）。

## 为什么不是 QClipboard

`QClipboard` 的模型是“一个应用一个剪贴板对象”（`QGuiApplication::clipboard()`），
信号只有 `changed(QClipboard::Mode)` / `dataChanged()`。Wayland 的
`wl_data_device.selection` 只发给拥有键盘焦点的客户端，所以后台的 `QClipboard`
收不到任何变化。Quickshell 自带的 `Quickshell.clipboardText` 同样是 `QClipboard`
包装，后台同样拿不到变化（实测：后台进程只打印了启动时的空字符串，没有触发任何
`changed`，而同一时刻 `wl-paste` 能读到新内容）。

真正让剪贴板管理器工作的是 data-control 协议：

* `ext-data-control-v1`：标准化协议，KWin、较新的 Hyprland 等；
* `wlr-data-control-unstable-v1`：wlroots 系（Hyprland、Sway、niri 等）。

本项目直接在 C++ 里实现这两个协议的客户端，使用独立的 `wl_display` 连接，
不经过 QtWaylandClient，也不启动任何辅助程序。

## 生命周期

这是一个**运行时服务**：只在宿主进程活着时管理剪贴板。

* 进程运行期间持有 `ext/wlr_data_control_source_v1`，别的应用粘贴时由它在
  `send` 里供数据；
* 进程退出或连接断开，合成器销毁 source，剪贴板内容随之失效；
* 不会 fork 常驻、不会留后台进程继续供数据，也没有任何磁盘持久化；
* 历史（包括图片字节）只保存在内存中，重新启动后从当前 selection 重新开始。

## 目录结构

```
.
├── CMakeLists.txt
├── LICENSE                          GPL-3.0
├── README.md
├── packaging/
│   └── quickshell-clipboard.spec    RPM spec
├── protocols/
│   ├── ext-data-control-v1.xml
│   └── wlr-data-control-unstable-v1.xml
└── src/
    ├── clipboardmanager.h/.cpp      QML 单例 + 历史模型 + 图片 provider
    └── waylandclipboard.h/.cpp      data-control 协议客户端
```

`ClipboardHistoryModel` 的 role 名与 QML delegate 的 `required property` 一致：
`uid / kind / clipText / imagePath / mime / imageHash / bytes / imgW / imgH / time`。
图片条目的 `imagePath` 是 `image://clipboard/<uid>`，由 `ClipboardImageProvider`
从内存解码，所以 `Image { source: model.imagePath }` 不需要改动。

## 实现要点

### 连接与协议选择

* `wl_display_connect(nullptr)` 建立独立连接，`QSocketNotifier` 接入 Qt 事件循环；
  `prepare_read` / `poll` / `read_events` / `dispatch_pending` 是标准的
  libwayland-Qt 集成方式，不会阻塞主线程。
* 第一次 `roundtrip` 只收集 global 名字，随后优先绑定 `ext-data-control-v1`，
  否则退回 `zwlr-data-control-unstable-v1`；再绑定第一个 `wl_seat` 并创建
  data device。协议不支持或连接断开时每 5 秒（无协议时 30 秒）重试。

### 读取 selection

* `data_offer` 建立 offer 对象并收集 MIME 类型，随后的 `selection` 消费它；
  `primary_selection` 的 offer 直接销毁（不镜像到历史）。
* MIME 优先级：`text/plain;charset=utf-8` → `UTF8_STRING` → `text/plain` →
  `TEXT` / `STRING`（按 Latin-1 解码）→ `text/uri-list`；没有文本类型时取
  `image/png` → `image/jpeg` → `image/webp` → `image/gif` → `image/bmp` → `image/tiff`。
* `offer.receive(mime, fd)` 通过 `pipe()` 传递：读端设为非阻塞并挂
  `QSocketNotifier`，写端在请求发出后立刻 `close()`。注意 libwayland 在序列化时
  会对 fd 做 `dup()`，如果保留自己那份写端，管道永远等不到 EOF，传输就会“读到了
  数据但永远不结束”。
* 单个条目上限 64 MiB；新 selection 到来时取消尚未完成的传输。

### 写回 selection

* 每次复制创建一个新的 `ext/wlr_data_control_source_v1`，先 `offer()` 所有 MIME
  类型，再 `set_selection()`；收到 `send(mime, fd)` 时把数据写入 fd 并关闭。
  旧 source 在收到 `cancelled` 后销毁。
* 进程忽略 `SIGPIPE`：请求方中途消失时 `write()` 不应杀死整个 shell。

### 历史行为

* 去重：同文本 / 同图片 SHA-256 只保留一条，并提升到列表顶部（uid 不变）；
* 上限：默认 50 条，超出后删除最旧的条目（内存随之释放）；
* 图片只存 `QByteArray`，尺寸用 `QImageReader` 只读文件头获得；
* 无 JSON、无临时文件、无图片目录；
* 写回 selection 后合成器会把新 selection 回送给我们，去重逻辑保证不会产生
  重复条目。

## 构建与安装

需要 `wayland-client` 开发包、`wayland-scanner` 和 Qt 6（Core/Gui/Qml/Quick）。

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
sudo cmake --install build --prefix /usr
```

默认安装到 `$prefix/lib64/qt6/qml/io/yigekuyou/clipboard/`（与发行版 Qt 6 的
QML 目录一致），也可以覆盖：

```sh
cmake -S . -B build -DCLIPBOARD_QML_DIR=lib/qml/io/yigekuyou/clipboard
```

安装后即可直接：

```qml
import io.yigekuyou.clipboard

Item {
    Repeater {
        model: ClipboardManager.history
        delegate: Text { required property string clipText; text: clipText }
    }
}
```

### Quickshell 集成

Quickshell 0.3.x 只把 `qs:@/` 加进 import 路径，没有提供注入系统 QML 路径的
配置项。系统安装后如果 Quickshell 仍找不到模块，可在配置目录放一个符号链接并
用相对目录导入：

```sh
ln -sfn /usr/lib64/qt6/qml/io/yigekuyou/clipboard \
        ~/.config/quickshell/ClipboardModule
```

```qml
// ~/.config/quickshell/Services/ClipboardService.qml
pragma Singleton
import QtQuick
import Quickshell
import "../ClipboardModule"

Singleton {
    readonly property var history: ClipboardManager.history
    readonly property int count: ClipboardManager.count
    readonly property string currentText: ClipboardManager.currentText
    readonly property string currentKind: ClipboardManager.currentKind
    readonly property int maxHistory: ClipboardManager.maxHistory

    signal historyUpdated()

    Connections {
        target: ClipboardManager
        function onHistoryUpdated() { root.historyUpdated() }
    }

    function copyToClipboard(text) { ClipboardManager.copyToClipboard(text) }
    function copyImage(path, mime) { ClipboardManager.copyImage(path, mime) }
    function removeEntry(uid) { ClipboardManager.removeEntry(uid) }
    function clearHistory() { ClipboardManager.clearHistory() }
}
```

或者启动 Quickshell 时设置 `QML_IMPORT_PATH=/usr/lib64/qt6/qml`，然后直接
`import io.yigekuyou.clipboard`。

## 打包 RPM

仓库自带 `packaging/quickshell-clipboard.spec`。

主要构建依赖：`cmake`、`gcc-c++`、`ninja`、`pkgconfig(wayland-client)`、
`qt6-base-devel`、`qt6-declarative-devel`。

## 验证

在真实合成器（Hyprland，`WAYLAND_DISPLAY=wayland-1`，同时提供 `ext-` 与
`zwlr-` 两套协议）上验证：

* 启动即捕获当前 selection；
* 外部应用改变文本后立即入库，`currentText` 同步；
* 外部应用复制 3×3 PNG：`mime=image/png`、`bytes=73`、`w/h=3`、SHA-256 正确；
* `Image { source: "image://clipboard/<uid>" }` 加载成功（`status=Ready`，尺寸正确）；
* `copyToClipboard()` 后 `wl-paste` 读到同样的文本；`copyImage("image://clipboard/1")`
  后 `wl-paste --type image/png` 读到 73 字节；
* 重复复制同一张图不会新增条目，只提升到顶部；
* `removeEntry()` / `clearHistory()` 正常，内存条目被释放；
* 整个运行过程不产生任何磁盘文件。

## 已知限制

* 历史是**每个 QML 引擎**一个实例。多个 Quickshell 进程同时导入时，会各自维护
  自己的历史（这符合“只在活着时管理”的定位）。
* `ext-data-control-v1` 目前仍是 wayland-protocols 的 staging 协议，接口版本为 1；
  `zwlr-data-control-unstable-v1` 绑定的是 v2（含 primary selection 事件）。
* 不支持 GNOME/Mutter：它既不提供 `ext-` 也不提供 `zwlr-` data-control。
* 图片历史占用内存；50 张大图的极端情况下内存占用会明显上升。
