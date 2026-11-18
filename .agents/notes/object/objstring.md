# ObjString 的已知拷贝风险

状态：未解决的 C++ 接口风险。源码见 `src/object/ObjString.hpp` 与 `src/object/ObjString.cpp`。

ObjString 持有长串缓冲的释放责任，但尚未显式删除复制构造与复制赋值；隐式复制只浅拷
long_chars_，两个副本析构会重复释放缓冲。对象应经指针借用、经 new_string 工厂创建，不按值复制。
该风险尚未由类型接口封死；本文记录问题，不将修复附带到其他字符串任务中。
