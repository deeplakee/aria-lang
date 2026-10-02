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
        MOVEMENT,
    };

    // ObjType 的类型名映射(PascalCase,唯一映射源):C++ 侧诊断与语言面(type() 内建/用户可见报错/debug_repr
    // 默认)统一走本函数;语言概念与实现名分叉处(协程)直接按语言词汇拼写。
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
            case ObjType::MOVEMENT:
                return "Coroutine"; // 语言概念是协程;MOVEMENT 只是 C++ 实现词根
            default:
                UNREACHABLE();
        }
    }

    // Object 自前向声明(concept 延迟求值 is_base_of,须先有名字)。
    class Object;

    // 供 is<T>/as<T> 与 GC::new_object<T> 约束 T 派生自 Object(is_base_of 延迟到实例化点)。
    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

    // 前向声明:虚函数签名只需不完整类型,完整定义见各自头。
    class GC;
    class ObjString;
    // AriaVM:基类默认体定义在 Object.cpp(vm.fail 是 AriaVM.hpp 内模板,而 AriaVM.hpp 经
    // ObjException.hpp 依赖本头、两头互不 include,出定义避环)。
    class AriaVM;

    // 工厂守卫纪律(**每方只守自己创建的**,全部 new_<type> 工厂通用 -- 各工厂不再复述):
    // 工厂不替调用方守卫**入参**(入参非本工厂创建);工厂内部新建的对象(便捷重载内 intern 的
    // 串)自带 make_guard 自守跨 new_object 顶 maybe_collect。故**调用方须在调用前自行根化自己
    // 传入的对象入参**(name / klass / module / super 等经 intern 或模块表皆为 weak root);工厂
    // 返回对象白色无根,建成即须发布进根(写回值栈槽 / 链入 VM 开链)。

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

        // 公共虚函数:具体 Object 子类型实现这些。

        // GC 标记阶段:遍历本对象的 Value/Object 子节点,调 gc.mark_value/mark_object。
        // 纯字符串等无子节点者空实现。
        virtual void trace(GC& gc) const noexcept = 0;

        // 壳对象的分配字节数(sizeof(壳),不含外挂 buffer),须与壳池配对:new_object 侧
        // ShellPool::alloc<T>(sizeof 上取整落格)、delete_object 侧 ShellPool::push(size(), obj)
        // -- 虚报外挂字节致分配/释放错格。子内存统一走虚析构(Array 成员自释放;非 Array 子内存如
        // ObjString 的 long_chars_ 由子类 ~dtor 经自持 GC* 释放);sweep_ 顺序:obj->~Object() ->
        // deallocate(壳)。
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

        // 成员/下标访问协议(LOAD/STORE_FIELD 族与 LOAD/STORE_INDEX 的分派点):语义在各宿主 override 一次收口,
        // 新增承载类型零 VM 改动;失败**自己 fail**、返回值只留信号(load 族 nullopt / store 族 false ⟺ 已 fail,
        // 失败出口 `return vm.fail(...);`);fail 文案渲染值走非重入渲染;调用方保证接收者「栈即根」。

        // 裸查找命名成员(PREPARE_METHOD 与实例 op_*_impl 统一入口):name 为 intern 串(=== 同指针查表)。
        // 返回查到的**原值**、永不铸 ObjBoundMethod、命中零分配:实例 fields 优先、miss 沿类链/表链取原值,
        // 故每次解析按当前类链即时生效。**槽 0 在消费时序里恒为接收者**(非方法成员被调用时它不是成员值)。
        // 错误契约:nullopt ⟺ 已 fail,miss 文案随宿主 override 就地烘焙;基类默认报 UndefinedProperty。
        [[nodiscard]]
        virtual Opt<Value> load_field(AriaVM& vm, ObjString* name);

        // 绑定读取命名成员(LOAD_FIELD / LOAD_THIS_FIELD 统一入口):同一趟裸查找;实例 fields 命中恒
        // 直读,类链命中经 is_method 现场铸 bound 绑 this、不写回 fields(类/父类改写对既有实例立即生效);
        // 内置类表条目全为原生方法,命中无条件绑定;基类默认 = 直接委托 load_field。错误契约同 load_field。
        [[nodiscard]]
        virtual Opt<Value> load_field_bound(AriaVM& vm, ObjString* name);

        // 写入命名成员(STORE_FIELD / STORE_THIS_FIELD 统一入口):基类默认报 "type X does
        // not support field access";ObjClass 落本类自身表恒成功;ObjInstance 动态字段永不失败;
        // ObjModule 恒拒(模块成员只读)。
        [[nodiscard]]
        virtual bool store_field(AriaVM& vm, ObjString* name, Value value);

        // 读取下标成员(LOAD_INDEX 统一入口):基类默认报 TypeMismatch "type X does not
        // support subscript access"。
        [[nodiscard]]
        virtual Opt<Value> load_index(AriaVM& vm, Value key);

        // 写入下标成员(STORE_INDEX 统一入口):契约同 store_field;其余类型落基类默认报错。
        [[nodiscard]]
        virtual bool store_index(AriaVM& vm, Value key, Value value);

        // 可重载算子/调用协议(取实现,不执行):回答「本对象上该算子对应的可调用值」,调用方取得后按调用形态调它。
        // 名字是语言级事实(拼写注册在 runtime/str_table.hpp)。基类默认直接 fail -- 默认不支持,子类型实现才不 fail。
        // 非 const(取实现可能物化绑定,同 load_field 族)。

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

        // is<T>() 一律 dynamic_cast 谓词(子类型均已落地,类型数个位数量级无需查表形态)。
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

    private:
        // GC 侵入式对象链的节点:sweep/free_all 直接取 &next_ 摘链(链头 objects_head_ 住 GC),
        // 字段级访问只能经友元;其余三字段对外只经访问器。
        friend class GC;

        Object(Object* next, const u32 hash, const ObjType type, const bool is_marked) noexcept :
            next_{next}, hash_{hash}, type_{type}, is_marked_{is_marked} {}

        Object* next_;      // GC 对象链下一节点(链尾 nullptr;链头住 GC)
        u32     hash_;      // 内容哈希(不可变对象)/ 地址哈希(可变对象),构造期烘焙
        ObjType type_;      // 子类型标识(构造期定,之后不可变)
        bool    is_marked_; // GC 标记位(is_marked/mark/unmark 维护)
    };

    inline void log_obj_alloc(Object* obj) {
        io::println("{:p} allocate bytes {} (Object {})", util::to_void_ptr(obj), obj->size(), obj->type_name());
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
