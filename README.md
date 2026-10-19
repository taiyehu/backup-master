# BackupMaster

BackupMaster 是《软件开发综合实验》的数据备份项目。当前版本 `0.3.0`
实现无压缩、无加密的单文件归档，包含 `backup`、`restore`、`list` 和
`verify` 四个命令。Windows 下支持文件/目录符号链接、目录联接、硬链接、
属主、DACL、文件属性和四类时间，并支持组合筛选备份内容。

## 后端依赖

第一版刻意保持最小依赖，以便独立实现核心逻辑，并避免第三方库影响课程扩展分。

| 依赖 | 用途 | 要求 |
|---|---|---|
| C++ 标准库 | 文件系统、二进制 I/O、容器 | C++17 |
| CMake | 构建与测试编排 | 3.20+ |
| G++/MinGW-w64 | Windows 编译器 | 支持 C++17 |
| CTest | 自动运行测试 | 随 CMake 提供 |
| Windows API | 重解析点、硬链接、安全描述符和文件时间 | Windows 系统自带 |

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

# 组合筛选；同类条件为“或”，不同类条件为“且”
./build-mingw/backup-master.exe backup data selected.bkm `
  --include-path "docs/**" --name "*.txt" `
  --modified-after "2026-01-01T00:00:00Z" `
  --min-size 1KiB --max-size 10MiB

# 查看归档内容
./build-mingw/backup-master.exe list <归档文件.bkm>

# 校验格式和每个文件的数据完整性
./build-mingw/backup-master.exe verify <归档文件.bkm>

# 恢复到不存在或为空的目录
./build-mingw/backup-master.exe restore <归档文件.bkm> <目标目录> `
  [--metadata best-effort|strict|none]
```

命令成功返回 `0`，参数错误返回 `2`，归档、文件系统或校验错误返回 `1`。

`backup` 的归档参数可以省略。省略时，程序会在当前工作目录的 `backup`
子目录中生成类似 `backup-20260818-143000.bkm` 的文件；同一秒内重名时会
自动追加数字后缀。

备份筛选选项可以重复：

- `--include-path`、`--exclude-path`：路径 glob，`*` 和 `?` 不跨越 `/`，`**`
  可以跨目录；匹配区分大小写。
- `--type`：`file`、`directory`、`file-symlink`、`directory-symlink`、
  `junction` 或 `hardlink`。
- `--name`：只匹配最后一个路径组件。
- `--created-after/before`、`--modified-after/before`、
  `--accessed-after/before`：UTC 时间 `YYYY-MM-DDTHH:MM:SSZ`；after 包含
  边界，before 不包含边界。
- `--owner`：Windows SID 或可解析的账户名。
- `--min-size`、`--max-size`：字节数或带 `B`、`KiB`、`MiB`、`GiB` 后缀。

排除规则优先级最高。被选中的符号链接或目录联接会自动补齐归档内目标和祖先目录；
若目标被显式排除则备份失败。没有条目匹配时不会生成空归档。

## 归档格式 v3

所有整数使用小端序。程序写入 v3，并继续读取 v1 和 v2 归档：

```text
文件头：魔数 "BKMASTR1" + 版本号 + 条目数

v3 条目：类型(u8) + 路径长度(u32) + 元数据长度(u32)
        + 数据长度(u64) + FNV-1a 64 校验值(u64)
        + UTF-8 相对路径 + 元数据 + 条目数据
```

类型 `1` 到 `6` 分别表示普通文件、目录、文件符号链接、目录符号链接、目录联接和
硬链接。引用型条目使用数据区保存 UTF-8 目标路径；校验值覆盖元数据和条目数据。
每个条目的固定开销为 25 字节，不含路径、元数据和数据。

符号链接和目录联接目标必须对应归档中的条目。目录联接只接受源树内目标，恢复时
重映射到新目标目录；硬链接只引用归档中的普通文件。程序拒绝路径穿越、外部联接、
引用循环、重复路径、未知类型、异常长度、截断、尾随数据和校验失败。

Windows 元数据包括创建、访问、修改、变更时间，文件属性，以及包含 owner、primary
group 和 DACL 的自相对安全描述符，不保存 SACL。默认恢复模式是 `best-effort`：
内容成功恢复，无法应用的元数据输出警告；`strict` 将其视为失败并回滚，`none` 跳过
元数据。符号链接创建仍可能需要开发者模式或相应权限。

FNV-1a 用于发现意外损坏，不属于密码学安全哈希。后续加密版本会增加强完整性认证。

## 当前边界

- 非 Windows 平台可以构建并处理普通文件及现有便携类型，但 Windows 安全描述符和
  目录联接只能在 Windows 上恢复。
- 设备文件以及符号链接、目录联接之外的重解析点不受支持。
- 筛选和归档路径匹配区分大小写，包括 Windows。
- 尚未实现压缩、加密、增量备份和 GUI。
- 备份过程中若源文件发生变化，任务会失败并删除未完成归档。
