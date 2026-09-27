# external -- 第三方库

| 库 | 上游 | 版本 | License | 用途 |
| --- | --- | --- | --- | --- |
| mimalloc | https://github.com/microsoft/mimalloc | v3.5.3 | MIT | GC 层分配器，默认启用（`ARIA_USE_MIMALLOC=OFF` 退回 std::malloc 家族） |
| isocline | https://github.com/daanx/isocline | v1.1.0 | MIT | REPL 行编辑 |
| googletest | https://github.com/google/googletest | v1.14.0 | BSD-3-Clause | 单元测试框架（`BUILD_GMOCK=OFF`，configure 零网络依赖） |

三个库均为裁剪后的 vendored 副本，接线方式与裁剪约束见根目录 `CMakeLists.txt` 对应块。
