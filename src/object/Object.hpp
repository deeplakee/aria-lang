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

    class AriaVM;
    class GC;
    class Object;
    class ObjString;
    class ObjFunction;
    class ObjNativeFn;
    class ObjUpvalue;
    class ObjClosure;
    class ObjClass;
    class ObjInstance;
    class ObjBoundMethod;
    class ObjList;
    class ObjMap;
    class ObjModule;
    class ObjRange;
    class ObjException;
    class ObjIterator;
    class ObjMovement;

    // 供 is<T>/as<T> 与 GC::new_object<T> 约束 T 派生自 Object(is_base_of 延迟到实例化点)。
    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

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
        // 指针相等不走本函数。前置条件: other 非空 -- 恒经 value_equal 的 Obj 臂 as_obj()
        // 传入(from_obj 的 DEBUG 断言兜底),override 入口 ASSERT 布防。**契约:须 GC-pure** --
        // EQUAL/NOT_EQUAL 在弹出的 off-stack 裸局部上经 value_equal 调用本函数,若触发 collect
        // 会回收操作数成悬垂。递归比较子值者(容器)入口另须挂 EqualGuard 防环:比较链重遇同对
        // 即视为相等(正则树同构),守卫分配走 std::allocator 不触 GC,比较中途不致 collect。
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

        // 类型判定 = if constexpr 按 T 编译期选 tag,运行期单比较(两配置同价零 RTTI)。前置条件:
        // this 非空 -- is_obj 门后的解码指针恒满足,from_obj 的 DEBUG 断言兜底。分辨率 =
        // ObjType,共享 tag 的族(迭代器四子类)只可按族基类查询;新增 ObjType 须在此扩臂,漏臂由
        // 末尾 UNREACHABLE 兜底。子类型禁定义 is/as/try_as 同名成员--成员调用从派生类作用域起
        // 查找,同名即遮蔽本三件套。
        template<DerivedFromObj T>
        [[nodiscard]]
        bool is() const noexcept {
            if constexpr (std::is_same_v<T, ObjString>) {
                return type() == ObjType::STRING;
            } else if constexpr (std::is_same_v<T, ObjFunction>) {
                return type() == ObjType::FUNCTION;
            } else if constexpr (std::is_same_v<T, ObjNativeFn>) {
                return type() == ObjType::NATIVE_FN;
            } else if constexpr (std::is_same_v<T, ObjUpvalue>) {
                return type() == ObjType::UPVALUE;
            } else if constexpr (std::is_same_v<T, ObjClass>) {
                return type() == ObjType::CLASS;
            } else if constexpr (std::is_same_v<T, ObjInstance>) {
                return type() == ObjType::INSTANCE;
            } else if constexpr (std::is_same_v<T, ObjBoundMethod>) {
                return type() == ObjType::BOUND_METHOD;
            } else if constexpr (std::is_same_v<T, ObjList>) {
                return type() == ObjType::LIST;
            } else if constexpr (std::is_same_v<T, ObjMap>) {
                return type() == ObjType::MAP;
            } else if constexpr (std::is_same_v<T, ObjModule>) {
                return type() == ObjType::MODULE;
            } else if constexpr (std::is_same_v<T, ObjRange>) {
                return type() == ObjType::RANGE;
            } else if constexpr (std::is_same_v<T, ObjIterator>) {
                return type() == ObjType::ITERATOR;
            } else if constexpr (std::is_same_v<T, ObjException>) {
                return type() == ObjType::EXCEPTION;
            } else if constexpr (std::is_same_v<T, ObjClosure>) {
                return type() == ObjType::CLOSURE;
            } else if constexpr (std::is_same_v<T, ObjMovement>) {
                return type() == ObjType::MOVEMENT;
            } else {
                UNREACHABLE();
            }
        }

        // 前置条件:调用前已经 is<T>() / switch(type()) 确认动态类型匹配--NDEBUG 下是
        // 裸 static_cast,不校验;DEBUG 下 dynamic_cast 兜底(不匹配返 nullptr 可暴露)。
        template<DerivedFromObj T>
        [[nodiscard]]
        T* as() noexcept {
#ifdef NDEBUG
            return static_cast<T*>(this);
#else
            return dynamic_cast<T*>(this);
#endif
        }

        // const 重载:const this -> const T*(DEBUG 下 dynamic_cast 校验,与 as() 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        const T* as() const noexcept {
#ifdef NDEBUG
            return static_cast<const T*>(this);
#else
            return dynamic_cast<const T*>(this);
#endif
        }

        // 检查式转换(try_as = is+as 合一):动态类型匹配返回转型指针,否则 nullptr。「守卫后使用」
        // 场景类型只写一次,消除 is<>/as<> 双类型参数漂移;纯谓词用 is<T>,switch(type()) 臂内等
        // 静态已知场合用 as<T>。前置条件同 is<T>()(this 非空)。
        template<DerivedFromObj T>
        [[nodiscard]]
        T* try_as() noexcept {
            return is<T>() ? as<T>() : nullptr;
        }

        // const 重载:const this -> const T*(与 as() const 对称)。
        template<DerivedFromObj T>
        [[nodiscard]]
        const T* try_as() const noexcept {
            return is<T>() ? as<T>() : nullptr;
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

    // 具体子类型的 tag 事实表:ObjType 标识 + 报错用小写限定词(读作「某个 list 下标」,与类型
    // 名的 PascalCase 分属两域)。消费方 = 类型守卫与按类报错;与 is<T> 的分派表是同一 tag↔类
    // 关联的两向。表对五内建类封闭,新增守卫类别须在此扩臂。
    struct ObjTag {
        ObjType    tag;
        StringView word;
    };

    template<DerivedFromObj T>
    constexpr ObjTag obj_tag() {
        if constexpr (std::is_same_v<T, ObjList>) {
            return {ObjType::LIST, "list"};
        } else if constexpr (std::is_same_v<T, ObjString>) {
            return {ObjType::STRING, "string"};
        } else if constexpr (std::is_same_v<T, ObjMap>) {
            return {ObjType::MAP, "map"};
        } else if constexpr (std::is_same_v<T, ObjRange>) {
            return {ObjType::RANGE, "range"};
        } else if constexpr (std::is_same_v<T, ObjIterator>) {
            return {ObjType::ITERATOR, "iterator"};
        }
        UNREACHABLE();
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
