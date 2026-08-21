#ifndef ARIA_OBJECT_HPP
#define ARIA_OBJECT_HPP

#include <format>
#include <type_traits>

#include "common.hpp"
#include "util/io.hpp"
#include "util/util.hpp"

namespace aria {

    // Object 子类型标识(普通 enum class)。
    // 转字符串见 to_string(ObjType);Object::type_ 持本类型,Object::type() 返回之。
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
    };

    // 对象类型可读名(如 "String"/"NativeFunction"),供日志与默认 to_string 渲染。
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
                return "NativeFunction";
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

    // GC 前向声明：Object 的虚函数 trace/destroy 以 GC& 为形参,此处仅需不完整类型即可。
    // 完整定义见 memory/GC.hpp(子类 .cpp include 后才能调用 GC 方法)。
    class GC;

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

        // 本对象的实际分配字节数(含 FAM/SSO 外挂),供 sweep_ 释放壳用。必须返回真实大小。
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

        // 对象的可读描述(Python 风格 `<Type at 0xaddr>`)。基类默认按 to_string(type_)
        // + 对象地址渲染;有更具体内容语义的子类型按需 override(如 ObjString 渲染字符内容)。
        // 与 to_string(ObjType) 的区别:前者是类型名的静态枚举映射,本方法产出"这个对象"的描述。
        [[nodiscard]]
        virtual String to_string() const {
            return std::format("<{} at {:p}>", aria::to_string(type_), util::to_void_ptr(this));
        }


        ////////////////////////////

        // TODO: 子类型落地后，is<T>() 可改用 ObjType 查表取代 dynamic_cast。
        template<DerivedFromObj T>
        [[nodiscard]]
        static bool is(const Object* o) noexcept {
            return dynamic_cast<const T*>(o) != nullptr;
        }

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

        Object* next_;
        u32     hash_;
        ObjType type_;
        bool    is_marked_;

    private:
        Object(Object* next, u32 hash, ObjType type, bool is_marked) noexcept :
            next_{next}, hash_{hash}, type_{type}, is_marked_{is_marked} {}
    };

    inline void log_obj_alloc(Object* obj) {
        io::println("{:p} allocate bytes {} (Object {})", util::to_void_ptr(obj), obj->size(), to_string(obj->type()));
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
