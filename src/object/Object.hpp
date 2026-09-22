#ifndef ARIA_OBJECT_HPP
#define ARIA_OBJECT_HPP

#include <format>
#include <type_traits>

#include "common.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
// Value(成员访问/运算符协议虚函数的签名需要完整类型):Value.hpp -> boxing 头 -> common.hpp,
// 不依赖 Object,无 include 环。
#include "value/Value.hpp"

namespace aria {

    // Object 子类型标识;类型名映射见 to_string(ObjType)。
    enum class ObjType : u8 {
        BASE,
        STRING,
        FUNCTION,
        NATIVE_FN,
        UPVALUE,
        CLASS,
        INSTANCE,
        BOUND_METHOD,
        LIST,
        MAP,
        MODULE,
        RANGE,
        ITERATOR,
        EXCEPTION,
        CLOSURE,
    };

    // ObjType 的静态可读名映射(PascalCase);Object::type_name() 委托本函数,日志与默认渲染复用。
    [[nodiscard]]
    constexpr StringView to_string(const ObjType kind) noexcept {
        switch (kind) {
            case ObjType::BASE:
                return "Base";
            case ObjType::STRING:
                return "String";
            case ObjType::FUNCTION:
                return "Function";
            case ObjType::NATIVE_FN:
                return "NativeFn";
            case ObjType::UPVALUE:
                return "Upvalue";
            case ObjType::CLASS:
                return "Class";
            case ObjType::INSTANCE:
                return "Instance";
            case ObjType::BOUND_METHOD:
                return "BoundMethod";
            case ObjType::LIST:
                return "List";
            case ObjType::MAP:
                return "Map";
            case ObjType::MODULE:
                return "Module";
            case ObjType::RANGE:
                return "Range";
            case ObjType::ITERATOR:
                return "Iterator";
            case ObjType::EXCEPTION:
                return "Exception";
            case ObjType::CLOSURE:
                return "Closure";
            default:
                UNREACHABLE();
        }
    }

    // Object 自前向声明:DerivedFromObj concept 在 class Object 定义之前引用 Object 名,
    // 需先声明(concept 不在此求值 is_base_of,延迟到实例化点 Object 已完整)。
    class Object;

    // 供 is<T>/as<T> 与 GC::new_object<T> 约束 T 派生自 Object(is_base_of 延迟到实例化点,
    // 届时 Object 已完整)。
    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

    // 以下前向声明:虚函数签名只需不完整类型,完整定义见各自头(子类 .cpp include 后才调用方法)。
    class GC;
    class ObjString;
    // AriaVM:基类默认体定义在 Object.cpp(vm.fail 是 AriaVM.hpp 内模板,而 AriaVM.hpp 经
    // ObjException.hpp 依赖本头、两头互不 include,出定义避环)。
    class AriaVM;

    class Object {
    public:
        Object() = delete;

        // 内容哈希型(ObjString/ObjRange 等不可变对象):显式传算好的内容哈希。
        Object(const u32 hash, const ObjType type) noexcept : Object{nullptr, hash, type, false} {}
        // 地址哈希型(可变对象默认):用对象地址算哈希(this 在 init list 中仅取地址,合法)。
        explicit Object(const ObjType type) noexcept : Object{nullptr, util::hash_addr(this), type, false} {}

        virtual ~Object() = default;

        [[nodiscard]]
        u32 hash() const noexcept {
            return hash_;
        }

        [[nodiscard]]
        ObjType type() const noexcept {
            return type_;
        }

        // 对象类型可读名:委托 to_string(ObjType),非虚(纯由 type_ 决定)。
        [[nodiscard]]
        constexpr StringView type_name() const noexcept {
            return aria::to_string(type_);
        }

        [[nodiscard]]
        bool is_marked() const noexcept {
            return is_marked_;
        }

        void mark() noexcept { is_marked_ = true; }

        void unmark() noexcept { is_marked_ = false; }

        ////////////////////////////
        //
        // 公共虚函数:具体 Object 子类型实现这些。
        //

        // GC 标记阶段:遍历本对象的 Value/Object 子节点,调 gc.mark_value/mark_object。
        // 纯字符串等无子节点者空实现。
        virtual void trace(GC& gc) const noexcept = 0;

        // 壳对象的分配字节数(sizeof(壳),不含外挂 buffer),须与 GC::new_object 的
        // allocate<u8>(sizeof(T)) 配对 -- 虚报外挂字节致分配/释放错配。子内存统一走虚析构
        //(Array 成员自释放;非 Array 子内存如 ObjString 的 long_chars_ 由子类 ~dtor 经自持
        // GC* 释放);sweep_ 顺序:obj->~Object() -> deallocate(壳)。
        [[nodiscard]]
        virtual usize size() const noexcept = 0;

        // 内容相等(== 语义),默认地址相等;ObjString 等按内容 override。=== 对 Obj 一律
        // 指针相等不走本函数。**契约:须 GC-pure** -- EQUAL/NOT_EQUAL 在弹出的 off-stack
        // 裸局部上经 value_equal 调用本函数,若触发 collect 会回收操作数成悬垂。递归比较
        // 子值者(容器)入口另须挂 EqualGuard 防环:比较链重遇同对即视为相等(正则树同构),
        // 守卫分配走 std::allocator 不触 GC,比较中途不致 collect。
        [[nodiscard]]
        virtual bool equals(const Object* other) const noexcept {
            return this == other;
        }

        // 调试渲染(repr 位),基类默认地址型 `<Type at 0xaddr>`。**override 契约 = 纯 C++
        // 惰性渲染**:只读自身成员造返回串,绝不执行 aria 字节码/调可重入 VM 的路径 --
        // format_value_debug / trace_execution 等在 dispatch_loop 内的调试上下文经虚分派
        // 调用,防重入由本契约维护(override 集合编译期封闭)。递归渲染子值者(容器)入口
        // 另须挂 PrintGuard 防环:元素重遇在印对象即截断 "[...]",否则无限递归栈溢出。
        [[nodiscard]]
        virtual String debug_repr() const {
            return std::format("<{} at {:p}>", type_name(), util::to_void_ptr(this));
        }

        // 可读描述(str 位),基类默认 = debug_repr;显示与调试分叉的子类型两者都 override
        //(当前唯一 ObjString:显示原文/调试带引号转义)。
        [[nodiscard]]
        virtual String to_string() const {
            return debug_repr();
        }

        //////////////////////////
        // 成员/下标访问协议(LOAD/STORE_FIELD 族与 LOAD/STORE_INDEX 的分派点)
        //
        // VM 不按子类型 switch 分型 --内建类型与用户类的成员语义在各自 override 一次收口,
        // 新增承载类型零 VM 改动。错误通道对齐 native fn 契约:签名收 AriaVM& 单一句柄,
        // 协议失败**自己 fail**(载荷直接入挂起寄存器,无返回值在途的白色无根窗口),返回值
        // 只留信号:load 族 Opt<Value> 的 nullopt ⟺ 已 fail(nil 命中亦 somed)、store 族
        // bool 的 false ⟺ 已 fail,失败出口一律 `return vm.fail(...);`(FailSignal 按站点
        // 返回类型转换)。
        //   - 文案由最知道语境的一方就地烘焙(越界含长度/键错误含键值);组合场景(实例委托
        //     类链、super 站点)直接委托 ObjClass::load_field,miss 的类措辞随协议传播。
        //   - 纪律:①fail 文案渲染值走非重入的 format_value_debug,不用可重载 to_string;
        //     ②至多 fail 一次、fail 后立即返回;③协议内可分配(绑定/装箱),调用方(VM)
        //     保证接收者「栈即根」(peek 不弹)。
        //////////////////////////

        // 读取命名成员(LOAD_FIELD / LOAD_THIS_FIELD 统一入口):name 为 intern 串(===
        // 同指针查表)。基类默认报 UndefinedProperty "X has no member 'y'";override 见
        // ObjInstance / ObjClass,默认体在 Object.cpp。
        [[nodiscard]]
        virtual Opt<Value> load_field(AriaVM& vm, ObjString* name);

        // 命名成员读取的不绑定形态(PREPARE_METHOD 统一入口):与 load_field 同一趟查找,但
        // **永不铸 ObjBoundMethod**、命中即原值直出;错误契约同 load_field(nullopt ⟺ 已
        // fail,文案随宿主 override 就地烘焙)。消费方两类:①VM 的 run_prepare_method -- 实参
        // 求值**之前**调本缝取被调值;②实例 11 个 op_*_impl -- 按算子/调用钩子名取 `__add__`/
        // `__call__` 等实现。
        // 调用时序与槽位:run_prepare_method 把返回值压在接收者之上,CALL_METHOD 再把实参整体
        // 下移一格补掉它,调用区回到 [recv, a1..aN],槽 0 保持接收者原样不动 -- 这是不铸 bound
        // 的关键:方法命中时 call_bound_method 自会用 bound 的 receiver 覆写槽 0;内置类表的
        // 原生函数恰好**正需要**槽 0 = receiver(其 this 兼返回槽,call_native 从不碰槽 0);
        // 字段里的可调用值/静态槽值走闭包或原生调用、不读槽 0。故本缝不涉槽位约定(「非方法成员
        // 被调用时槽 0 为接收者而非成员值」这一形态差异见 bytecode-instruction-set.md §5.6)。
        // **基类默认 = load_field**(类/模块等未 override 者:其读取本就不绑定,照读路径取值),
        // 命中即该值本身,miss 文案随宿主烘焙。
        // **实例与内置容器/迭代器各自 override**(同一个理由:读取路径要绑定,本缝永不铸
        // ObjBoundMethod)--
        //   实例:fields 命中优先,否则沿类链取**原值**(方法戳闭包不绑定,方法体从槽 0 读 this),
        //         零分配且每次按当前类链解析(改类/父类方法立即生效,见 ObjInstance.hpp);
        //   内置容器/迭代器:查自身 bootstrap 类表取原生值(条目恒为原生、恒绑定),miss 的类措辞
        //          fail 随 ObjClass::load_field 透传。
        [[nodiscard]]
        virtual Opt<Value> load_field_unbound(AriaVM& vm, ObjString* name);

        // 写入命名成员(STORE_FIELD / STORE_THIS_FIELD 统一入口):基类默认报 "type X does
        // not support field access";ObjClass 落本类自身表恒成功;ObjInstance 动态字段永不失败。
        [[nodiscard]]
        virtual bool store_field(AriaVM& vm, ObjString* name, Value value);

        // 读取下标成员(LOAD_INDEX 统一入口):基类默认报 TypeMismatch "type X does not
        // support subscript access";override 见 ObjList/ObjMap/ObjString。
        [[nodiscard]]
        virtual Opt<Value> load_index(AriaVM& vm, Value key);

        // 写入下标成员(STORE_INDEX 统一入口):契约同 store_field;override 见
        // ObjList/ObjMap/ObjString,其余类型落基类默认报错。
        [[nodiscard]]
        virtual bool store_index(AriaVM& vm, Value key, Value value);

        //////////////////////////
        // 可重载算子协议与调用协议(取实现,不执行)
        //
        // 每个算子/调用一个虚函数,回答「**本对象上该算子对应的可调用值**」-- 不是算好的结果:调用方
        // (VM 的 run_binary_operator/run_negate/call_value)拿到后按调用形态调它(调用区槽 0 保持
        // receiver),故实现既可是内建原生、也可是用户方法/闭包。名字是语言级事实(kOp*Name;调用钩子
        // `__call__`)。
        // **基类默认直接 fail**(`type X does not support '<钩子名>'`;调用用 CallNonCallable),与
        // load_field/store_field 等基类默认同款「默认不支持,子类型实现才不 fail」。实现者:①实例 --
        // 11 个 override 各按名 load_field_unbound(实例 fields 可遮蔽,再类链);②内置 string -- 5 个算子
        // 直给实现格 String*Fn(免查找);③其余类型不实现即报错(方法仍在类表里,`"a".__add__("b")`
        // 读路径不变)。非 const(取实现可能物化绑定,与 load_field/load_field_unbound 同族)。
        //////////////////////////

        [[nodiscard]]
        virtual Opt<Value> op_add_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_sub_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_mul_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_div_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_mod_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_less_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_less_equal_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_greater_impl(AriaVM& vm);

        [[nodiscard]]
        virtual Opt<Value> op_greater_equal_impl(AriaVM& vm);

        // 一元取负(-x)。
        [[nodiscard]]
        virtual Opt<Value> op_negate_impl(AriaVM& vm);

        // 函数调用
        [[nodiscard]]
        virtual Opt<Value> op_call_impl(AriaVM& vm);

        ////////////////////////////

        // is<T>() 目前一律 dynamic_cast;性能敏感后可改 ObjType 查表(子类型均已落地)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static bool is(const Object* object) noexcept {
            return dynamic_cast<const T*>(object) != nullptr;
        }

        // 前置条件:调用前已经 is<T>() / switch(type()) 确认动态类型匹配--NDEBUG 下是
        // 裸 static_cast,不校验;DEBUG 下 dynamic_cast 兜底(不匹配返 nullptr 可暴露)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static T* as(Object* object) noexcept {
#ifdef NDEBUG
            return static_cast<T*>(object);
#else
            return dynamic_cast<T*>(object);
#endif
        }

        // const 重载:const Object* -> const T*(DEBUG 下 dynamic_cast 校验,与 as(Object*) 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static const T* as(const Object* object) noexcept {
#ifdef NDEBUG
            return static_cast<const T*>(object);
#else
            return dynamic_cast<const T*>(object);
#endif
        }

        // 检查式转换(try_as = is+as 合一):动态类型匹配返回转型指针,否则 nullptr(含
        // object 为 null)。「守卫后使用」场景类型只写一次,消除 is<>/as<> 双类型参数漂移;
        // 纯谓词用 is<T>,switch(type()) 臂内等静态已知场合用 as<T>。
        template<DerivedFromObj T>
        [[nodiscard]]
        static T* try_as(Object* object) noexcept {
            return is<T>(object) ? as<T>(object) : nullptr;
        }

        // const 重载:const Object* -> const T*(与 as(const Object*) 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static const T* try_as(const Object* object) noexcept {
            return is<T>(object) ? as<T>(object) : nullptr;
        }

        Object* next_;
        u32     hash_;
        ObjType type_;
        bool    is_marked_;

    private:
        Object(Object* next, const u32 hash, const ObjType type, const bool is_marked) noexcept :
            next_{next}, hash_{hash}, type_{type}, is_marked_{is_marked} {}
    };

    inline void log_obj_alloc(Object* obj) {
        io::println("{:p} allocate bytes {} (Object {})", util::to_void_ptr(obj), obj->size(), obj->type_name());
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
