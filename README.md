# BackupMaster

BackupMaster 是《软件开发综合实验》的数据备份项目。当前版本 `0.1.0`
实现无压缩、无加密的单文件归档，包含 `backup`、`restore`、`list` 和
`verify` 四个命令。

## 后端依赖

第一版刻意保持最小依赖，以便独立实现核心逻辑，并避免第三方库影响课程扩展分。

| 依赖 | 用途 | 要求 |
|---|---|---|
| C++ 标准库 | 文件系统、二进制 I/O、容器 | C++17 |
| CMake | 构建与测试编排 | 3.20+ |
| G++/MinGW-w64 | Windows 编译器 | 支持 C++17 |
| CTest | 自动运行测试 | 随 CMake 提供 |

运行时不依赖第三方动态库。当前开发环境已具备 CMake 4.2、G++ 14.2 和
GNU Make 4.4，因此无需额外下载安装。

## 构建

在 PowerShell 中运行：

```powershell
./build.ps1
```

Release 构建：

```powershell
./build.ps1 -Configuration Release
```

生成的程序位于 `build-mingw/backup-master.exe`。

## 使用

```powershell
# 将目录备份为一个归档文件；为防误覆盖，归档文件必须不存在
./build-mingw/backup-master.exe backup <源目录> [归档文件.bkm]

# 查看归档内容
./build-mingw/backup-master.exe list <归档文件.bkm>

# 校验格式和每个文件的数据完整性
./build-mingw/backup-master.exe verify <归档文件.bkm>

# 恢复到不存在或为空的目录
./build-mingw/backup-master.exe restore <归档文件.bkm> <目标目录>
```

命令成功返回 `0`，参数错误返回 `2`，归档、文件系统或校验错误返回 `1`。

`backup` 的归档参数可以省略。省略时，程序会在当前工作目录的 `backup`
子目录中生成类似 `backup-20260818-143000.bkm` 的文件；同一秒内重名时会
自动追加数字后缀。

## 归档格式 v1

所有整数使用小端序。归档由一个固定文件头和若干条目组成：

```text
文件头：魔数 "BKMASTR1" + 版本号 + 条目数

条目：类型 + UTF-8 路径长度 + 数据大小 + FNV-1a 64 校验值
      + UTF-8 相对路径 + 文件数据
```

目录条目没有文件数据。恢复时会拒绝绝对路径、路径穿越、重复路径、未知条目类型、
数据截断、尾随数据和校验失败的归档。

FNV-1a 用于发现意外损坏，不属于密码学安全哈希。后续加密版本会增加强完整性认证。

## 当前边界

- 仅处理普通文件和目录；符号链接及设备文件暂不支持。
- 尚未保存权限、属主和修改时间。
- 尚未实现压缩、加密、增量备份和 GUI。
- 备份过程中若源文件发生变化，任务会失败并删除未完成归档。
