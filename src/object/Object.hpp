#ifndef ARIA_OBJECT_HPP
#define ARIA_OBJECT_HPP

#include <format>
#include <type_traits>

#include "aria.hpp"
#include "common.hpp"
#include "object/ObjType.hpp"
#include "util/io.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class Object;
    class ObjString;
    class ObjFnProto;
    class ObjUpvalue;
    class ObjFunction;
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
    class ObjStream;

    template<typename T>
    concept DerivedFromObj = std::is_base_of_v<Object, T>;

    class Object {
    public:
        Object() = delete;

        // 不可变对象可传入内容哈希；其余对象默认按地址哈希。
        Object(const u32 hash, const ObjType type) noexcept : Object{nullptr, hash, type, false} {}
        explicit Object(const ObjType type) noexcept : Object{nullptr, util::hash_addr(this), type, false} {}

        virtual ~Object() = default;

        // GC 链与外部引用依赖对象的地址身份，禁止复制或迁移对象本体。
        Object(const Object&)            = delete;
        Object& operator=(const Object&) = delete;
        Object(Object&&)                 = delete;
        Object& operator=(Object&&)      = delete;

        [[nodiscard]]
        u32 hash() const noexcept {
            return hash_;
        }

        [[nodiscard]]
        ObjType type() const noexcept {
            return type_;
        }

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

        // 标记本对象引用的 GC 子对象；无子对象时为空实现。
        virtual void trace(GC& gc) const noexcept = 0;

        // 仅返回壳大小，不含外挂缓冲；ShellPool 依此选择释放格位。
        [[nodiscard]]
        virtual usize size() const noexcept = 0;

        // 实现须不触发 GC：比较操作数可能已离栈。容器递归比较须用 EqualGuard 防环。
        // other 须非空；此协议服务于 ==，=== 对象比较始终按指针。
        [[nodiscard]]
        virtual bool equals(const Object* other) const noexcept {
            return this == other;
        }

        // 调试渲染不得执行用户代码或重入 VM；容器递归渲染须用 PrintGuard 防环。
        [[nodiscard]]
        virtual String repr() const {
            return std::format("<{} at {:p}>", type_name(), util::to_void_ptr(this));
        }

        // 访问协议失败时须先 ctx.fail，再返回 nullopt/false；调用方负责保活接收者。

        // 仅查所属类的成员链，忽略实例字段和类、模块的本地命名空间，供隐式钩子查询。
        [[nodiscard]]
        virtual Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name);

        // 普通成员裸查询：name 须为 intern 串；返回原值，不分配绑定方法。
        [[nodiscard]]
        virtual Opt<Value> load_field(AriaExeContext& ctx, ObjString* name);

        // 默认绑定 is_method 值；实例字段、类和模块命名空间中的值保持原样。
        [[nodiscard]]
        virtual Opt<Value> load_field_bound(AriaExeContext& ctx, ObjString* name);

        [[nodiscard]]
        virtual bool store_field(AriaExeContext& ctx, ObjString* name, Value value);

        [[nodiscard]]
        virtual Opt<Value> load_index(AriaExeContext& ctx, Value key);

        [[nodiscard]]
        virtual bool store_index(AriaExeContext& ctx, Value key, Value value);

        // 按 ObjType 查询；共享 tag 的迭代器子类只能按族基类查询。
        // 子类不得定义同名 is/as/try_as，以免遮蔽此接口。
        template<DerivedFromObj T>
        [[nodiscard]]
        bool is() const noexcept {
            if constexpr (std::is_same_v<T, ObjString>) {
                return type() == ObjType::STRING;
            } else if constexpr (std::is_same_v<T, ObjFnProto>) {
                return type() == ObjType::FN_PROTO;
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
            } else if constexpr (std::is_same_v<T, ObjFunction>) {
                return type() == ObjType::FUNCTION;
            } else if constexpr (std::is_same_v<T, ObjMovement>) {
                return type() == ObjType::MOVEMENT;
            } else if constexpr (std::is_same_v<T, ObjStream>) {
                return type() == ObjType::STREAM;
            } else {
                STATIC_UNREACHABLE("is: no mapping for this Obj subclass");
            }
        }

        // 调用前须已确认类型匹配；Release 使用 static_cast，Debug 使用 dynamic_cast。
        template<DerivedFromObj T>
        [[nodiscard]]
        T* as() noexcept {
#ifdef NDEBUG
            return static_cast<T*>(this);
#else
            return dynamic_cast<T*>(this);
#endif
        }

        template<DerivedFromObj T>
        [[nodiscard]]
        const T* as() const noexcept {
#ifdef NDEBUG
            return static_cast<const T*>(this);
#else
            return dynamic_cast<const T*>(this);
#endif
        }

        template<DerivedFromObj T>
        [[nodiscard]]
        T* try_as() noexcept {
            return is<T>() ? as<T>() : nullptr;
        }

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
    // 关联的两向。新增守卫类别须在此扩臂,漏臂编译期报错。
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
        } else if constexpr (std::is_same_v<T, ObjStream>) {
            return {ObjType::STREAM, "stream"};
        } else {
            STATIC_UNREACHABLE("obj_tag: no mapping for this Obj subclass");
        }
    }

} // namespace aria

#endif // ARIA_OBJECT_HPP
