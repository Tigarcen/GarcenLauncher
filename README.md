## 中文

### 编辑工具
- Visual Studio Code
- 插件：clangd

### 编译工具
- mingw64 g++

### 调试平台
- Windows 11 25H2

### 编译命令
在项目根目录执行下面这一行命令（先编译版本资源，再编译源码并链接）：

```powershell
windres src/version.rc -o version.o; if ($?) { g++ src/main.cpp src/DrawImage.cpp src/ControlConfig.cpp src/ActionArea.cpp src/List.cpp src/Menu.cpp version.o -o GarcenLauncher.exe -std=c++17 -O2 -Wall -Wextra -mwindows -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -luser32 -lgdi32 -lgdiplus -ldwmapi -lwindowsapp -lole32 -lshell32 -lwinmm }
```

#### 图片资源
PNG 图标通过 [src/version.rc](src/version.rc) 以 `RCDATA` 形式编进 exe（`assets/trash.png`、`trash_on.png`、`opacity.png`、`theme.png`、`drop.png`、`box.png`、`web.png`、`win.png`），运行时用 `DrawPngResource` 从资源加载，所以**发布时不需要带 assets 目录**。

- `assets/win.png` 由 `CreatePngResourceIcon` 转成窗口/任务栏和托盘使用的 `HICON`；它只是 `RCDATA`，没有注册 `ICON` 资源，因此**不会修改 exe 文件图标**。
- 资源 ID 定义在 [src/DrawImage.h](src/DrawImage.h) 的 `AssetResource` 枚举，必须和 `version.rc` 里的数字一致。
- `version.rc` 中的文件路径是相对 **执行 windres 时的当前目录**，所以要在项目根目录编译；路径用 `/` 分隔，写成 `assets\trash.png` 会被预处理器当成转义（`\t`）。

#### Git Bash 下编译失败的处理
如果 g++ 静默失败（无任何报错），原因是 PATH 中 `Git\mingw64\bin` 里的旧版 `libwinpthread-1.dll` 排在工具链之前，缺少 cc1plus 需要的 `clock_gettime64` 导出函数。把工具链目录提到 PATH 最前即可：

```shell
PATH="/c/ProgramData/mingw64/mingw64/bin:$PATH" windres src/version.rc -o version.o && PATH="/c/ProgramData/mingw64/mingw64/bin:$PATH" g++ src/main.cpp src/DrawImage.cpp src/ControlConfig.cpp src/ActionArea.cpp src/List.cpp src/Menu.cpp version.o -o GarcenLauncher.exe -std=c++17 -O2 -Wall -Wextra -mwindows -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -luser32 -lgdi32 -lgdiplus -ldwmapi -lwindowsapp -lole32 -lshell32 -lwinmm
```

### 功能
#### 拖入文件或文件夹
- 拖住文件或文件夹，然后使用快捷键，就能把文件或文件夹拖入添加到列表中
- 分组
  - 把单项拖到其他项上，就能合并成一个分组
- 排序
  - 拖动图标，就能改变列表中的顺序
- 删除
  - 拖动单项或者分组到垃圾桶图标上，松手就能删除选中的项
- 重命名
  - 鼠标左击选中的项，就能重命名选中的项
  - 分组的重命名也可以在分组内双击标题来修改
- 文件位置
  - 鼠标右击选中的项，就能打开选中的项所在文件夹
- 透明度
  - 窗口左下角有一上一下的两个按钮，用于调整透明度
- 亮色和暗色主题
  - 窗口右下角有一个切换主题的按钮，点击后就能切换主题

#### 互斥锁
- 程序启动时，会检查是否有其他实例在运行
- 如果有其他实例在运行，就会让正在运行的实例显示窗口

#### RAM占用
- 实测596×480的画满图标等信息的窗口，专用RAM占用15MB左右

-------------------------------------------------------------------------------------

## English

### Editor
- Visual Studio Code
- Extension: clangd

### Compiler
- MinGW64 g++

### Tested Platform
- Windows 11 25H2

### Build Command
Run the following command in the project root directory (compile the version resource first, then compile and link the source files):

```powershell
windres src/version.rc -o version.o; if ($?) { g++ src/main.cpp src/DrawImage.cpp src/ControlConfig.cpp src/ActionArea.cpp src/List.cpp src/Menu.cpp version.o -o GarcenLauncher.exe -std=c++17 -O2 -Wall -Wextra -mwindows -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -luser32 -lgdi32 -lgdiplus -ldwmapi -lwindowsapp -lole32 -lshell32 -lwinmm }
```

#### Image Resources
PNG icons are embedded into the executable as `RCDATA` through [src/version.rc](src/version.rc) (`assets/trash.png`, `trash_on.png`, `opacity.png`, `theme.png`, `drop.png`, `box.png`, `web.png`, and `win.png`). They are loaded from resources at runtime using `DrawPngResource`, so **the assets directory is not required when distributing the application**.

- `assets/win.png` is converted by `CreatePngResourceIcon` into an `HICON` used by the window, taskbar, and system tray. Since it is only stored as `RCDATA` and is not registered as an `ICON` resource, it **does not change the executable's file icon**.
- Resource IDs are defined in the `AssetResource` enumeration in [src/DrawImage.h](src/DrawImage.h) and must match the numbers in `version.rc`.
- File paths in `version.rc` are relative to the **current directory when `windres` is executed**, so run the command from the project root directory. Use `/` as the path separator; in `assets\trash.png`, the preprocessor interprets `\t` as an escape sequence.

#### Troubleshooting Build Failures in Git Bash
If `g++` fails silently without displaying any errors, an older `libwinpthread-1.dll` in `Git\mingw64\bin` appears before the toolchain in `PATH` and lacks the `clock_gettime64` export required by `cc1plus`. Prepend the toolchain directory to `PATH` to fix this:

```shell
PATH="/c/ProgramData/mingw64/mingw64/bin:$PATH" windres src/version.rc -o version.o && PATH="/c/ProgramData/mingw64/mingw64/bin:$PATH" g++ src/main.cpp src/DrawImage.cpp src/ControlConfig.cpp src/ActionArea.cpp src/List.cpp src/Menu.cpp version.o -o GarcenLauncher.exe -std=c++17 -O2 -Wall -Wextra -mwindows -municode -static -static-libgcc -static-libstdc++ -DUNICODE -D_UNICODE -luser32 -lgdi32 -lgdiplus -ldwmapi -lwindowsapp -lole32 -lshell32 -lwinmm
```

### Features
#### Adding Files or Folders
- Drag a file or folder and use the shortcut to add it to the list.
- Groups
  - Drag an item onto another item to merge them into a group.
- Reordering
  - Drag icons to change their order in the list.
- Deletion
  - Drag an item or group onto the trash icon and release it to delete the selected item.
- Renaming
  - Left-click a selected item to rename it.
  - To rename a group, you can also double-click its title while inside the group.
- File Location
  - Right-click a selected item to open its containing folder.
- Opacity
  - Use the up and down buttons in the lower-left corner of the window to adjust its opacity.
- Light and Dark Themes
  - Click the theme button in the lower-right corner of the window to switch themes.

#### Single-Instance Mutex
- At startup, the application checks whether another instance is already running.
- If another instance is running, its window is displayed instead.

#### RAM Usage
- Measured on a 596×480 window with icons and information filled in, the application consumes around 15MB of RAM.