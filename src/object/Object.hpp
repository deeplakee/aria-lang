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

    // Object 子类型标识(普通 enum class)。
    // 类型名转换见 to_string(ObjType) / Object::type_name()。
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

    // 对象类型可读名(PascalCase,如 "String"/"NativeFn"/"Module"):ObjType 枚举的静态映射,
    // 供日志、默认 to_string 渲染与 Object::type_name() 复用。单一来源--Object::type_name() 委托本函数。
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

    // 供 Object 的静态成员模板 is<T>/as<T> 与 GC::new_object<T> 约束 T:派生自 Object
    //(is_base_of 求值时机见上)。
    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

    // GC 前向声明：Object 的虚函数 trace 以 GC& 为形参、虚析构 ~Object() 无参,此处仅需不完整类型即可。
    // 完整定义见 memory/GC.hpp(子类 .cpp include 后才能调用 GC 方法)。
    class GC;

    // ObjString 前向声明:成员访问协议虚函数以 ObjString* 为形参,头内仅需不完整类型
    //(子类型 override 的实现见各自 .cpp,届时 include ObjString.hpp)。
    class ObjString;

    // AriaVM 前向声明:成员/下标访问协议与算术协议的虚函数以 AriaVM&(错误通道句柄)为首参,
    // 头内仅需不完整类型;基类默认体定义在 Object.cpp(vm.fail 是 AriaVM.hpp 内模板,而
    // AriaVM.hpp 经 ObjException.hpp 依赖本头、两头互不 include,出声明避环)。
    class AriaVM;

    class Object {
    public:
        Object() = delete;

        // 内容哈希型(ObjString/ObjRange 等不可变对象):显式传算好的内容哈希。
        Object(u32 hash, ObjType type) noexcept : Object{nullptr, hash, type, false} {}
        // 地址哈希型(可变对象默认):用对象地址算哈希(this 在 init list 中仅取地址,合法)。
        explicit Object(ObjType type) noexcept : Object{nullptr, util::hash_addr(this), type, false} {}

        virtual ~Object() = default;

        [[nodiscard]]
        u32 hash() const noexcept {
            return hash_;
        }

        [[nodiscard]]
        ObjType type() const noexcept {
            return type_;
        }

        // 对象类型可读名(PascalCase):委托 to_string(ObjType) 取 type_ 的枚举映射。非虚--
        // 类型名纯由 type_ 决定,子类无需 override;Value 层 type_name(Value) 的 Obj 分支调本方法。
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

        // 壳对象的分配字节数(sizeof(壳),不含外挂 buffer),供 sweep_ 释放壳用。
        // 必须与 GC::new_object 的 allocate<u8>(sizeof(T)) 配对--虚报外挂字节会致分配/释放错配。
        //
        // 子内存释放统一走虚析构:Array 成员靠自身 dtor 自释放(持 GC*),
        // 非 Array 子内存(如 ObjString 的 long_chars_)在子类 ~dtor 里经自己持的 GC* 释放。
        // sweep_ 顺序:obj->~Object() -> deallocate(壳)。
        [[nodiscard]]
        virtual usize size() const noexcept = 0;

        // 内容相等(== 语义):默认地址相等(this==other)。子类型按内容语义 override(ObjString
        // 比字符内容;函数/类等无"内容"概念者保持默认)。与 value_identical(===) 的区别:
        // === 对 Obj 一律指针相等,不走本虚函数。由 value_equal(==) 在 Obj 分支调用。
        //
        // 契约:须 GC-pure -- 不得触发 GC 回收(不调可能 maybe_collect 的路径)。原因:VM 的
        // EQUAL/NOT_EQUAL 在弹出的 a/b(off-stack 裸局部)上经 value_equal 调用本函数;若本函数
        // collect,会回收 a/b 指向的对象 -> 悬垂。当前子类型实现均为纯比较;hash() 返回构造期
        // 算好的 hash_ 天然 pure;to_string 走 std::allocator 亦不触回收。
        [[nodiscard]]
        virtual bool equals(const Object* other) const noexcept {
            return this == other;
        }

        // 对象的调试渲染(repr 位):各子类型 override 自己的 debug 文案(ObjString 带引号转义 /
        // ObjFunction `<fn name>` / ObjUpvalue `<upvalue>` 等),基类默认 = 地址型
        // `<Type at 0xaddr>` 兜底。**override 契约 = 纯 C++ 惰性渲染**:只读自身成员造返回串
        // (String 走 std::allocator,不触 GC 回收),绝不执行 aria 字节码 / 调 call_value 等
        // 可重入 VM 的路径;override 集合编译期封闭(语言层无法新增 C++ 子类型),
        // format_value_debug / trace_execution / 反汇编常量池等 dispatch_loop 内调试上下文经
        // 虚分派调用安全(防重入由本契约维护)。
        [[nodiscard]]
        virtual String debug_repr() const {
            return std::format("<{} at {:p}>", type_name(), util::to_void_ptr(this));
        }

        // 对象的可读描述(str 位):基类默认 = debug_repr()(显示与调试同文案);显示与调试
        // 分叉的子类型两者都 override(当前唯一:ObjString--显示原文无引号、调试带引号转义)。
        // 与 type_name() 的区别:前者是类型名的静态枚举映射,本方法产出"这个对象"的描述。
        [[nodiscard]]
        virtual String to_string() const {
            return debug_repr();
        }

        ////////////////////////////
        // 成员/下标访问协议(LOAD/STORE_FIELD 族与未来 LOAD/STORE_INDEX 的分派点)
        //
        // 命名成员/下标的读写统一经对象虚函数协议分派,VM 不按子类型 switch 分型 --内建类型与
        // 用户类的成员语义在各自 override 里一次收口,新增承载类型零 VM 改动。
        //
        // 错误通道(对齐 native fn 契约):签名收 AriaVM& 单一状态句柄(分配经 vm.gc(),报错
        // 一行 vm.fail(code, fmt, ...)),协议失败时**自己 fail** --错误载荷直接入挂起错误
        // 寄存器,构造即被 VM 根 tracer 标根,无「返回值在途」的白色无根窗口;**返回值只留
        // 信号**:load 族返回 Opt<Value>,nullopt ⟺ 已 fail(nil 命中亦 somed);store 族返回
        // bool,false ⟺ 已 fail。失败出口一行化:override 一律 `return vm.fail(...);`
        //(fail 返回 FailSignal 哨兵,按所在函数返回类型隐式转换 --Opt 站点转 nullopt /
        // bool 站点转 false / 指针站点转 nullptr)。
        //   - 消息文案由最知道语境的一方**就地烘焙**(越界含长度/键错误含键值):各 override
        //     用自身细节拼,不经 VM 类别映射;组合场景(实例委托类链、super 站点)直接**委托
        //     协议** ObjClass::load_field --命中值原样回传、miss 的类措辞 fail 随协议传播
        //     (成员表在类链上,文案随宿主,组合方不重复烘焙)。
        //   - 契约纪律:①fail 文案渲染值一律走非重入的 format_value_debug,不用可重载的
        //     to_string(防未来语言级 __str__ 重入 VM);②至多 fail 一次、fail 后立即返回;
        //     ③协议内可分配(绑定/装箱),调用方(VM)须保证接收者「栈即根」(peek 不弹)
        //     跨协议内的 GC 点。
        //////////////////////////

        // 读取命名成员(LOAD_FIELD / LOAD_THIS_FIELD 统一入口):name 为 intern 串
        // (=== 同指针查表)。返回 Opt<Value>:somed = 读取结果(nil 命中亦 somed);
        // nullopt = 已 fail。基类默认(本类型无命名成员语义):报 UndefinedProperty
        // "X has no member 'y'"(对象描述经 debug_repr);override 见 ObjInstance /
        // ObjClass。基类默认体定义在 Object.cpp(见上 AriaVM 前向声明)。
        [[nodiscard]]
        virtual Opt<Value> load_field(AriaVM& vm, ObjString* name);

        // 写入命名成员(STORE_FIELD / STORE_THIS_FIELD 统一入口):返回 bool,false ⟺ 已 fail。
        // 基类默认:本类型不支持成员赋值,报 "type X does not support field access";
        // ObjClass 落本类自身表恒成功(继承名/新名新建键遮蔽);ObjInstance 动态字段
        // upsert 永不失败(恒 true)。
        [[nodiscard]]
        virtual bool store_field(AriaVM& vm, ObjString* name, Value value);

        // 读取下标成员(LOAD_INDEX 接线留容器里程碑):key 任意 Value(容器自定合法性与
        // 语义)。**备置 API,暂无 override 与调用方**--容器 override 直接 vm.fail 自选
        // 错误码、细节就地拼进文案。基类默认:本类型不支持下标读取,报 TypeMismatch
        // "type X does not support subscript access"。
        [[nodiscard]]
        virtual Opt<Value> load_index(AriaVM& vm, Value key);

        // 写入下标成员(STORE_INDEX 接线留容器里程碑):契约同 store_field(false ⟺ 已
        // fail)。备置 API,基类默认同 load_index 报不支持。
        [[nodiscard]]
        virtual bool store_index(AriaVM& vm, Value key, Value value);

        //////////////////////////
        // 可重载运算符协议(算术虚函数族,备置 API)
        //
        // lhs = this(本对象)、rhs = 另一操作数(任意 Value);const 纯计算不改接收者。
        // 错误通道契约同成员协议:签名收 AriaVM&(分配 vm.gc() / 报错 vm.fail),返回
        // Opt<Value>,nullopt ⟺ 已 fail。div/mod 的整除零/f64 IEEE 等数值细节属 VM 原语
        // 路径,重载方自定语义。
        //
        // **备置 API:接口已落地,暂无子类 override、暂无调用方**--VM 算术指令的接线留到
        // 容器里程碑/用户类运算符重载立项时:原语走原数值路径,对象操作数经本协议虚分派
        //(对象在左直调;在右的反射接法届时设计)。先备好接口,免得 VM 长出一组分型辅助方法。
        // 接线纪律:接收者与 rhs 须「栈即根」(peek 不弹)--协议 miss 路径 fail 与结果路径
        // 分配均触 maybe_collect,弹栈裸局部会被回收(与 equals 的 GC-pure 契约相对:后者
        // 在弹栈裸局部上被 EQUAL 调用、永不分配)。基类默认体定义在 Object.cpp,一律报
        // TypeMismatch "operator '...' requires numbers, got X and Y"(与 VM 原语路径文案
        // 一致)、op_negate 报 InvalidOperand "negate requires a number"。
        //////////////////////////

        [[nodiscard]]
        virtual Opt<Value> op_add(AriaVM& vm, Value rhs) const;

        [[nodiscard]]
        virtual Opt<Value> op_sub(AriaVM& vm, Value rhs) const;

        [[nodiscard]]
        virtual Opt<Value> op_mul(AriaVM& vm, Value rhs) const;

        [[nodiscard]]
        virtual Opt<Value> op_div(AriaVM& vm, Value rhs) const;

        [[nodiscard]]
        virtual Opt<Value> op_mod(AriaVM& vm, Value rhs) const;

        // 一元取负(-x):无 rhs。基类默认报 InvalidOperand "negate requires a number"。
        [[nodiscard]]
        virtual Opt<Value> op_negate(AriaVM& vm) const;

        //////////////////////////
        // 可调用协议(CALL 的对象侧分派点,备置 API)
        //
        // call_value 的 switch 对已实装可调用类型(CLOSURE/NATIVE_FN/CLASS/BOUND_METHOD)
        // 精确分派,其余类型落本协议基类默认(fail CallNonCallable);未来新增可调用对象类型
        // override 本方法即接入,无需改 call_value 的 switch。
        //
        // **签名与 ObjNativeFn 契约同构**(`NativeFn = bool (*)(AriaVM&, Span<Value>)`):
        // 调用区 [callee, a1..aN] 的可写视图 --slots[0] = callee(双职:被调者/返回槽,
        // 写返回值即覆写 slots[0])、slots[1..size()-1] = 实参(argc = slots.size() - 1)。
        // 执行型协议(进帧/改栈/写返回槽)与 op_* 算术族(纯计算)分属两族:返回 bool --
        // true = 成功(返回值已写 slots[0]),false = 已 fail(载荷在挂起寄存器);覆写槽 0
        // 的特殊语义(如实例化原位换实例作 this)由 override 自定。
        //////////////////////////

        [[nodiscard]]
        virtual bool op_call(AriaVM& vm, Span<Value> slots);


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
        Object(Object* next, u32 hash, ObjType type, bool is_marked) noexcept :
            next_{next}, hash_{hash}, type_{type}, is_marked_{is_marked} {}
    };

    inline void log_obj_alloc(Object* obj) {
        io::println("{:p} allocate bytes {} (Object {})", util::to_void_ptr(obj), obj->size(), obj->type_name());
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
