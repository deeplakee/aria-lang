# 编译器设计取舍

现行接口与语义约束见 [compile 参考](../../reference/compile.md)，赋值与循环 lowering 见
[lowering 规格](../../reference/specs/lowering.md)。

## superclass 常量池烘焙

不采用 Wren 式将 superclass 烘入共享函数原型常量池的方案。类在运行期定义，绑定时刻
需要额外回填；把 defining_class 标记复制给嵌套闭包，又会使 `is_method` 将其误判为实例方法。
当前 super 从直接方法闭包获取定义类；this 可沿局部捕获，嵌套函数需要 super 方法时先取值再捕获。

值填槽与 clox 的预占槽模型不同；批量关闭 upvalue 对齐 Lua OP_CLOSE，`resolve_upvalue`
采用逐层解析捕获链。跨语言参照只解释设计来源，不替代项目规格。
