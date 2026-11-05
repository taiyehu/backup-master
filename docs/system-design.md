# BackupMaster 系统设计（v0.4.0）

## 模块职责

- `archive_service`：备份、列表、校验、恢复的任务编排和失败回滚。
- `archive_format`：v1/v2/v3/v4 小端序编解码、长度上限和流式校验。
- `compression`：逐块 RLE 编解码及解压输出上限检查。
- `scanner` 与 `filter`：不跟随重解析点地扫描源树，组合筛选并补齐依赖。
- `metadata`：Windows 时间、属性、owner、primary group 和 DACL 的捕获、编码与恢复。
- `symlink` 与 `special_files`：符号链接、目录联接、文件 ID 和硬链接的平台操作。
- `manifest`：重复路径、父子关系、引用类型、目标存在性和引用环校验。

```mermaid
flowchart LR
    CLI --> Service[ArchiveService]
    Service --> Scanner
    Scanner --> Filter
    Scanner --> Metadata
    Scanner --> Links[Symlink / Junction / Hardlink]
    Service --> Format[Archive format v1-v4]
    Service --> Compression[RLE codec]
    Format --> Manifest
    Service --> Metadata
    Service --> Links
```

```mermaid
classDiagram
    class ArchiveService {
      +backup(source, options)
      +restore(archive, destination, options)
      +list(archive)
      +verify(archive)
    }
    class BackupOptions {
      +archive
      +filter
      +preserve_metadata
    }
    class RestoreOptions {
      +metadata_mode
    }
    ArchiveService ..> BackupOptions
    ArchiveService ..> RestoreOptions
    ArchiveService ..> CompressionCodec
```

## v3 数据布局

```text
archive_header = magic[8] + version:u32 + entry_count:u64
entry_header   = type:u8 + path_size:u32 + metadata_size:u32
               + data_size:u64 + checksum:u64
entry          = entry_header + path + metadata + data
```

所有整数按小端序保存。FNV-1a 64 从元数据开始，再连续计算条目数据。`list` 可以跳过
普通文件数据；`verify` 和 `restore` 必须流式读取全部数据。元数据和引用目标都有独立
上限，所有归档路径必须为安全 UTF-8 相对路径。

Windows 元数据块带平台号和模式版本，保存四个 `FILE_BASIC_INFO` 时间、文件属性、
owner SID 文本和自相对安全描述符。安全描述符只包含 owner、primary group 和 DACL。

## v4 压缩方案

全局文件头仍为 20 字节。v4 在 v3 固定条目头末尾增加 `codec:u8`、
`original_size:u64`，固定头共 34 字节。`codec=0` 表示原样数据，此时
`original_size=data_size`；`codec=1` 仅允许普通文件，表示 RLE。其余类型
必须为原样数据。校验值继续覆盖元数据和实际存储的数据字节。

RLE 按最多 64 KiB 的输入块独立编码。控制字节 0–127 后接 1–128 字节原文；
控制字节 128–255 后接一个重复字节，重复次数为控制字节减 127。连续三个及
以上相同字节优先形成重复包。编码后不小于原文时选择 `codec=0`。写入前先
流式计算编码长度及校验值，第二遍编码写入并比较，防止源文件变化。
解码时同时限制输入消费量和输出量，最终输出必须恰等于 `original_size`。
`verify` 和 `restore` 都完整解码；`list` 仍只跳过普通文件载荷。

## 特殊文件语义

```mermaid
flowchart TD
    Scan[扫描条目] --> Detect{类型识别}
    Detect --> File[普通文件 / 文件 ID]
    Detect --> Sym[符号链接]
    Detect --> Junction[IO_REPARSE_TAG_MOUNT_POINT]
    File --> Group[筛选后按文件 ID 分组]
    Group --> Canon[首路径保存数据]
    Group --> Hard[其余路径引用首路径]
    Sym --> Dependency[验证归档内目标]
    Junction --> Dependency
```

目录联接在归档中不保存原始绝对目标，而保存源树相对目标。恢复时按“目录、文件、
硬链接、符号链接/目录联接、元数据”的顺序执行，避免写入过程经过尚未验证的链接。
任一核心恢复步骤失败都会清理本次创建的内容。

## 筛选数据流

路径排除在遍历阶段剪枝。其余路径、类型、名字、时间、owner 和尺寸条件在候选项完成
类型及必要元数据采集后组合判断：同类规则为 OR，不同类规则为 AND。随后补齐祖先
目录以及被选链接的归档内目标；显式排除不得被依赖补齐覆盖。硬链接关系只在最终选择
集合中建立。
