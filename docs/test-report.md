# BackupMaster 测试报告（v0.4.0）

## 范围与追踪

| 需求 | 测试依据 |
|---|---|
| R-01 | 归档重名、筛选、源树内输出拒绝 |
| R-02 | 大段重复数据压缩率与恢复内容比较；普通数据往返 |
| R-03 | v1/v2/v3/v4 列表和引用目标断言 |
| R-04 | 危险路径、未知类型和版本、异常元数据、截断、校验失败、尾随数据、畸形 RLE |
| R-05 | 恢复数据错误回滚；元数据 best-effort、strict、none 模式 |
| R-06 | 手工构造的旧版归档与 v4 往返 |

自动测试位于 `tests/archive_tests.cpp`，由 `build.ps1` 调用 CTest。测试在唯一系统临时
目录中运行。Windows 符号链接创建测试在没有相应权限时跳过；其余类型按平台能力测试。
压缩编码在 64 KiB 输入块上独立运行，恢复时以头部原始长度为硬上限。

## 本次执行结果

- Windows MinGW Debug：`./build.ps1` 编译无告警，CTest 全部通过。
- Windows MinGW Release：`./build.ps1 -Configuration Release` 编译无告警，CTest 全部通过。
- Ubuntu WSL Debug：CMake 编译和 CTest 全部通过；Windows 与 WSL 文件时钟略有偏差，
  Make 提示 clock skew，未出现 C++ 编译告警。
- CLI：对 `docs` 执行 backup、verify、list、restore，并逐文件比较 SHA-256，结果一致。
