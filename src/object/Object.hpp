#ifndef ARIA_OBJECT_HPP
#define ARIA_OBJECT_HPP

#include <format>
#include <type_traits>

#include "common.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
// Value(成员访问/运算符协议虚函数的签名需要完整类型):Value.hpp -> boxing 头 -> common.hpp,
// 不依赖 Object,无 include 环;且子类型头早已经 value/AriaHashTable.hpp 等拉入 Value,对基类
// 头的暴露者非新增。突破旧「Object.hpp 不 include Value.hpp」约束的代价经评估为零(2026-09-10
// 虚函数协议上基类起)。
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

    // 供 Object 的静态成员模板 is<T>/as<T> 与 GC::new_object<T> 约束 T:派生自 Object。
    // std::is_base_of 的求值延迟到静态模板实例化时(Object 已完整)。
    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

    // GC 前向声明：Object 的虚函数 trace 以 GC& 为形参、虚析构 ~Object() 无参,此处仅需不完整类型即可。
    // 完整定义见 memory/GC.hpp(子类 .cpp include 后才能调用 GC 方法)。
    class GC;

    // ObjString 前向声明:成员访问协议虚函数以 ObjString* 为形参,头内仅需不完整类型
    //(子类型 override 的实现见各自 .cpp,届时 include ObjString.hpp)。
    class ObjString;

    // AriaVM 前向声明:成员/下标访问协议与算术协议的虚函数以 AriaVM& 为首参(错误通道
    // 句柄,2026-09-10 二次整改),头内仅需不完整类型 -- 同 ObjNativeFn 的 NativeFn 签名
    // 先例;基类默认体定义在 Object.cpp(vm.fail 是 AriaVM.hpp 内模板,而 AriaVM.hpp 经
    // ObjException.hpp 依赖本头、两头互不 include,出声明避环)。
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

        // 对象类型可读名(PascalCase,如 "String"/"NativeFn"/"Module"):委托 to_string(ObjType)
        // 取本对象 type_ 的枚举映射,供错误消息类型名打印与默认 to_string 渲染。非虚--类型名纯由 type_
        // 决定,子类无需 override。与 Value 层自由函数 type_name(Value) 配合:后者 Obj 分支调本方法取子类型。
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

        // 内容相等(== 语义):默认地址相等(this==other)。子类型按内容语义 override--
        // ObjString 比字符内容,未来 ObjList/ObjMap 递归逐元素;函数/类等无"内容"概念者
        // 保持默认。与 value_identical(===) 的区别:=== 对 Obj 一律指针相等,不走本虚函数。
        // 由 value_equal(==) 在 Obj 分支调用。
        //
        // 契约:须 GC-pure -- 不得触发 GC 回收(不调 new_object/new_string/HashTable::upsert
        // 等可能 maybe_collect 的路径)。原因:VM 的 EQUAL/NOT_EQUAL 在弹出的 a/b(off-stack 裸
        // 局部)上经 value_equal 调用本函数;若本函数 collect,会回收 a/b 指向的对象 -> 悬垂。
        // 当前所有子类型实现均为纯比较(无分配),满足契约。hash() 同理须 pure(其为非虚 getter
        // 返回构造期算好的 hash_,天然满足)。to_string 返回 String(std::string,经 std::allocator
        // 而非 GC),亦不触回收。
        [[nodiscard]]
        virtual bool equals(const Object* other) const noexcept {
            return this == other;
        }

        // 对象的调试渲染(repr 位):各具体子类型 override 实现自己的 debug 文案(ObjString
        // 字面量带引号转义 / ObjFunction `<fn name>` / ObjUpvalue `<upvalue>` 等),基类默认 =
        // 地址型 `<Type at 0xaddr>`(无更具体内容语义时的兜底)。虚函数,但 **override 契约 =
        // 纯 C++ 惰性渲染**:只读自身 C++ 成员造返回串(String 走 std::allocator,不触 GC 回收),
        // 绝不执行 aria 字节码 / 调 call_value 等可重入 VM 的路径。语言层无法新增 C++ 子类型,
        // override 集合编译期封闭,故 format_value_debug / trace_execution / 反汇编常量池等
        // dispatch_loop 内调试上下文经虚分派调用本方法安全,绝不触用户重载--「防重入」由本
        // 契约维护,不再依赖非虚分派(2026-09-10 起 debug 文案从 format_value_debug 的 ObjType
        // switch 下沉到各子类型,value 层不再认识具体子类型)。
        [[nodiscard]]
        virtual String debug_repr() const {
            return std::format("<{} at {:p}>", type_name(), util::to_void_ptr(this));
        }

        // 对象的可读描述(str 位):基类默认 = debug_repr()(显示与调试同文案);显示语义与
        // 调试分叉的子类型两者都 override(当前唯一:ObjString--显示原文无引号、调试带引号
        // 转义)。未来用户类 __str__ 落地时在 ObjInstance::to_string 分叉,调试位保持惰性不受
        // 影响。与 type_name() 的区别:前者是类型名的静态枚举映射,本方法产出"这个对象"的描述。
        [[nodiscard]]
        virtual String to_string() const {
            return debug_repr();
        }

        ////////////////////////////
        // 成员/下标访问协议(LOAD/STORE_FIELD 族与未来 LOAD/STORE_INDEX 的分派点)
        //
        // 命名成员/下标的读写统一经对象虚函数协议分派,VM 不按子类型 switch 分型 --内建类型与
        // 用户类的成员语义在各自 override 里一次收口,新增承载类型零 VM 改动(M5 阶段 2
        // 整改定案;错误通道随 2026-09-10 二次整改翻为下述 vm.fail 模型,原「类别返回 +
        // VM 统一烘焙」契约与三态 StoreResult 一并退役)。
        //
        // 错误通道(对齐 native fn 契约,同构 CPython PyErr 模型):签名收 AriaVM& 单一状态
        // 句柄(分配经 vm.gc(),报错一行 vm.fail(code, fmt, ...)),协议失败时**自己 fail**
        // --错误载荷直接入 *current_ 挂起错误寄存器,构造即入寄存器即被 VM 根 tracer 标根,
        // 无「返回值在途」的白色无根窗口;**返回值只留信号**:load 族返回 Opt<Value>,
        // nullopt ⟺ 已 fail(somed 才是命中,命中值为 nil 亦 somed);store 族返回 bool,
        // false ⟺ 已 fail。与 native fn 的 bool 契约、call_value 族同构,runtime 在途错误
        // 自此只有寄存器一种载体。失败出口经 FailSignal 哨兵一行化:override 一律
        // `return vm.fail(...);`(fail 返回 FailSignal,按所在函数返回类型隐式转换 --
        // Opt 站点转 nullopt / bool 站点转 false / 指针站点转 nullptr,契约拼写由签名决定、
        // 写法全族统一)。
        //   - 消息文案由最知道语境的一方**就地烘焙**(越界含长度/键错误含键值,与 CPython
        //     listobject.c 就地拼消息同构):各 override 用自身细节拼,不经 VM 类别映射;
        //     组合场景(实例委托类链、super 站点)直接**委托协议** ObjClass::load_field --
        //     命中值原样回传、miss 的类措辞 fail 随协议传播(成员表在类链上,文案随宿主,
        //     组合方不重复烘焙)。
        //   - 契约纪律:①fail 文案渲染值一律走非重入的 format_value_debug,不用可重载的
        //     to_string(防未来语言级 __str__ 重入 VM);②至多 fail 一次、fail 后立即返回;
        //     ③协议内可分配(绑定/装箱),调用方(VM)须保证接收者「栈即根」(peek 不弹)
        //     跨协议内的 GC 点。
        //////////////////////////

        // 读取命名成员(LOAD_FIELD / LOAD_THIS_FIELD 统一入口):name 为 intern 串
        // (=== 同指针查表)。返回 Opt<Value>:somed = 读取结果(值为 nil 亦 somed);
        // nullopt = 已 fail。基类默认(本类型无命名成员语义):报 UndefinedProperty
        // "X has no member 'y'"(对象描述经 debug_repr)。已落地 override
        // (M5):ObjInstance(fields 命中优先,未命中委托类协议 load_field 沿链读穿透,
        // 可调用值绑 this
        // 并回填 fields 缓存,miss 随类措辞 fail)、ObjClass(沿链读穿透直读,不绑定不
        // 缓存,miss 以类措辞 fail)。基类默认体定义在 Object.cpp(见上 AriaVM 前向声明)。
        [[nodiscard]]
        virtual Opt<Value> load_field(AriaVM& vm, ObjString* name);

        // 写入命名成员(STORE_FIELD / STORE_THIS_FIELD 统一入口):value 为赋的值。
        // 返回 bool:false ⟺ 已 fail。基类默认:本类型不支持成员赋值,报 "type X does
        // not support field access";ObjClass 落本类自身表恒成功(继承名/新名新建键遮蔽,
        // 动态新增允许,2026-09-11 改定);ObjInstance 动态字段 upsert 永不失败(恒 true)。
        [[nodiscard]]
        virtual bool store_field(AriaVM& vm, ObjString* name, Value value);

        // 读取下标成员(LOAD_INDEX 接线留容器里程碑):key 任意 Value(容器自定合法性与
        // 语义,如 list 整数下标 / map 任意键)。**备置 API,暂无 override 与调用方**--
        // 二次整改后的接线形态:容器 override 直接 vm.fail 自选错误码(IndexOutOfBounds /
        // KeyError 等已预置),错误细节(越界的下标值与容器长度)就地拼进文案,不再需要
        // 「类别 + VM 烘焙」的中间协议。基类默认:本类型不支持下标读取,报 TypeMismatch
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
        // 算术运算的虚函数协议:lhs = this(本对象)、rhs = 另一操作数(任意 Value);
        // const 纯计算不改接收者。错误通道契约同成员协议(2026-09-10 二次整改):签名收
        // AriaVM&(分配 vm.gc() / 报错 vm.fail),返回 Opt<Value>,nullopt ⟺ 已 fail。
        // div/mod 的整除零/f64 IEEE 等数值细节属 VM 原语路径,重载方自定语义。
        //
        // **备置 API(2026-09-10 定案,二次整改随错误通道翻型):现在落地接口,暂无子类
        // override、暂无调用方**--VM 算术指令(run_binary_numeric)的接法留到容器里程碑/
        // 用户类运算符重载立项时:原语走原数值路径,对象操作数经本协议虚分派(对象在左直调;
        // 在右的反射接法届时设计)。现在备好接口,避免重蹈「LOAD_FIELD 忘了规划虚函数、
        // VM 长出一组分型辅助方法」的覆辙。
        // 接线纪律:接收者与 rhs 须「栈即根」(peek 不弹)--协议 miss 路径 fail 与结果
        // 路径分配(如未来 list+list 新建)均触 maybe_collect,弹栈裸局部会被回收(与
        // equals 的 GC-pure 契约相对:后者在弹栈裸局部上被 EQUAL 调用、永不分配,是协议
        // 边界上的反例参照)。基类默认体定义在 Object.cpp,一律报 TypeMismatch
        // "operator '...' requires numbers, got X and Y"(与 VM 原语路径文案一致)、
        // op_negate 报 InvalidOperand "negate requires a number"。
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


        ////////////////////////////

        // is<T>() 目前一律 dynamic_cast;性能敏感后可改 ObjType 查表(子类型均已落地)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static bool is(const Object* o) noexcept {
            return dynamic_cast<const T*>(o) != nullptr;
        }

        // 前置条件:调用前已经 is<T>() / switch(type()) 确认动态类型匹配--NDEBUG 下是
        // 裸 static_cast,不校验;DEBUG 下 dynamic_cast 兜底(不匹配返 nullptr 可暴露)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static T* as(Object* o) noexcept {
#ifdef NDEBUG
            return static_cast<T*>(o);
#else
            return dynamic_cast<T*>(o);
#endif
        }

        // const 重载:const Object* -> const T*。供 equals(const Object*) 等 const 上下文
        // 收口类型转换,避免裸 static_cast(DEBUG 下 dynamic_cast 校验,与 as(Object*) 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static const T* as(const Object* o) noexcept {
#ifdef NDEBUG
            return static_cast<const T*>(o);
#else
            return dynamic_cast<const T*>(o);
#endif
        }

        // 检查式转换(try_as = is+as 合一):动态类型匹配返回转型指针,否则 nullptr(含 o 为 null)。
        // 「守卫后使用」成对场景的一步形态:类型只写一次,消除 is<>/as<> 双类型参数漂移笔误;
        // DEBUG 下单次 dynamic_cast(优于成对写法的两次)。纯谓词(不取指针)用 is<T>;
        // switch(type()) 臂内等静态已知场合用 as<T>。
        template<DerivedFromObj T>
        [[nodiscard]]
        static T* try_as(Object* o) noexcept {
            return is<T>(o) ? as<T>(o) : nullptr;
        }

        // const 重载:const Object* -> const T*(与 as(const Object*) 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        static const T* try_as(const Object* o) noexcept {
            return is<T>(o) ? as<T>(o) : nullptr;
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
