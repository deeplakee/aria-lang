#include "runtime/ListMethods.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // ---- list 方法实现(NativeFn 方法调用形态:slots[0] = receiver 兼返回槽,读 slots[1..]) ----

        // push(x) -> nil:追加 x 到末尾(任意 Value)。返回 nil(Python append 同款,变更方法
        // 不鼓励链式)。
        bool push_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "push expects 1 argument, got {}", argc);
            }
            // 绑定路径契约:slots[0] 恒本 list(仅经 load_field 绑定触达),DEBUG 下 as 走
            // dynamic_cast 校验。
            const auto list = Object::as<ObjList>(slots[0].as_obj());
            list->elements().push(slots[1]); // trivial 分配不触 GC
            slots[0] = Value::nil_val();
            return true;
        }

        // pop() -> 末元素:移除并返回末元素(任意 Value);空表报 IndexOutOfBounds。
        bool pop_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "pop expects no arguments, got {}", argc);
            }
            const auto list     = Object::as<ObjList>(slots[0].as_obj());
            auto&      elements = list->elements();
            if (elements.empty()) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "pop from empty list");
            }
            slots[0] = elements[elements.size() - 1];
            elements.pop();
            return true;
        }

        struct ListMethodEntry {
            StringView name;
            NativeFn   fn;
        };

        // iter() -> 迭代器:铸造 ObjListIterator(list 与其迭代器成对,铸造口按类型解开
        // receiver)。GC 时序:list 在 slots[0] 于栈根,迭代器白色建成**先写回槽发布再返回**,
        // 中间无 GC 点;此后 list 经迭代器 trace 可达。
        bool iter_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "iter expects no arguments, got {}", argc);
            }
            const auto list = Object::as<ObjList>(slots[0].as_obj());
            slots[0]        = Value::from_obj(new_list_iterator(vm.gc(), list));
            return true;
        }

        // list 方法表:注册进 List bootstrap 类(kBuiltins 同款循环)。注册名经 new_native_fn
        // 的 StringView 重载 intern,与 CodeGen LOAD_FIELD 发射的同名常量同指针,查表按指针命中。
        constexpr ListMethodEntry kListMethods[] = {
                {"push", push_fn},
                {"pop", pop_fn},
                {"iter", iter_fn},
        };

    } // namespace

    void register_list_methods(GC& gc, ObjClass* klass) {
        for (const auto& [name, fn]: kListMethods) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

} // namespace aria
