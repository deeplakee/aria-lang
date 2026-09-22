#include "runtime/builtins/ListBuiltins.hpp"

#include <algorithm>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // ---- list 方法实现(NativeFn 方法调用形态:slots[0] = receiver 兼返回槽,读 slots[1..]) ----
        // 惯例:receiver 解开后直取 elements() 绑为 list(单次使用也不内联;仅 iter 需要
        // ObjList* 本体传给迭代器);段搬移类变更(insert/remove_at)收口 Array 原语,方法体
        // 只余域检查与调用。

        // push(x) -> nil:追加 x 到末尾(任意 Value)。返回 nil(Python append 同款,变更方法
        // 不鼓励链式)。
        bool push_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "push expects 1 argument, got {}", argc);
            }
            // 绑定路径契约:slots[0] 恒本 list(仅经 load_field 绑定触达),DEBUG 下 as 走
            // dynamic_cast 校验。
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            list.push(slots[1]); // trivial 分配不触 GC
            slots[0] = Value::nil_val();
            return true;
        }

        // pop() -> 末元素:移除并返回末元素(任意 Value);空表报 IndexOutOfBounds(fail-fast,
        // nil 哨兵不可行 --list 可合法存 nil)。任意位置移除走 remove_at。
        bool pop_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "pop expects no arguments, got {}", argc);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            if (list.empty()) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "pop from empty list");
            }
            slots[0] = list[list.size() - 1];
            list.pop(); // 搬移时 receiver 已离 slots[0],trivial 无 GC 点
            return true;
        }

        // insert(i, x) -> nil:在位置 i 之前插入 x(任意 Value)。位置语义(Python insert 同款):
        // i == 元素数即追加;负数从尾计数、指「该下标元素之前」(-1 即末元素之前),合法域
        // [-(size), size] 收口 util::resolve_position(resolve_index 的姊妹函数,上界放宽到
        // 追加位)。段右移腾位收口 Array::insert。
        bool insert_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.fail(ErrorCode::WrongArity, "insert expects 2 arguments, got {}", argc);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "insert index must be an integer, got {}", type_name(slots[1]));
            }
            auto&      list     = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto position = util::resolve_position(slots[1].as_int(), list.size());
            if (!position) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "insert index out of range");
            }
            list.insert(*position, slots[2]);
            slots[0] = Value::nil_val();
            return true;
        }

        // remove(x) -> Bool:移除**全部** == 命中元素(Ruby Array#delete 同款移全;只要一处
        // 用 find + remove_at 组合),命中 true、未命中 false 不报错 --miss 走返回值,与
        // find 返 -1 / contains 返 false 同族;错误通道留给无信号通道的结构性失败(如空表
        // pop)。收口 AriaArray::remove(一趟稳定压缩)。
        bool remove_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "remove expects 1 argument, got {}", argc);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]   = Value::from_bool(list.remove(slots[1]));
            return true;
        }

        // remove_at(i) -> i 处元素:按位置移除并返回(pop 的任意位置形,负数从尾计数、与下标
        // 读写同语义,越界报原始键值)。段左移补位收口 Array::remove_at,trivial 搬移无 GC 点
        //(receiver 已离 slots[0])。
        bool remove_at_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "remove_at expects 1 argument, got {}", argc);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "remove_at index must be an integer");
            }
            auto&      list = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto slot = util::resolve_index(slots[1].as_int(), list.size());
            if (!slot) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "remove_at index out of range");
            }
            slots[0] = list[*slot];
            list.remove_at(*slot);
            return true;
        }

        // clear() -> nil:清空(长度归零,容量保留)。重绑 xs = [] 换新表,别名仍见旧内容 --本方法
        // 供共享可变状态原地清空。
        bool clear_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "clear expects no arguments, got {}", argc);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            list.clear();
            slots[0] = Value::nil_val();
            return true;
        }

        // 排序域:比较算子的可比较域两支(数值 / 字符串)加域外单列(NotComparable 覆盖 nil/
        // bool/容器等)。sort 域检的判定底座:域外元素在排序前即拒绝。
        enum class SortDomain : u8 {
            Number,
            String,
            NotComparable,
        };

        SortDomain sort_domain(const Value value) noexcept {
            if (is_num(value)) {
                return SortDomain::Number;
            }
            if (try_obj<ObjString>(value) != nullptr) {
                return SortDomain::String;
            }
            return SortDomain::NotComparable;
        }

        // sort() -> nil:就地升序(Python list.sort 同款,变更方法返 nil)。域 = 比较算子的可比较
        // 域(全数值或全字符串),先整体域检再排序 --比较器免「不可比」分支,排序中途无失败路径;
        // 升序判定收口 value_less(value 层自然序,域外未定义的契约由域检保证)。GC 走查:域检与
        // 比较均无 GC 分配;stable_sort 的临时缓冲走 std 内存(非 GC 堆),receiver 在 slots[0] 未
        // 覆写。稳定序:等值元素(如 int 1 与 f64 1.0)保输入相对序。
        bool sort_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "sort expects no arguments, got {}", argc);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            if (!list.empty()) {
                // 首元素定域,两类皆非即报(只含其类型);其余元素逐个须同域,破域(含两类皆非)
                // 报首元素与破类元素两类类型。
                const auto domain = sort_domain(list[0]);
                if (domain == SortDomain::NotComparable) {
                    return vm.fail(ErrorCode::TypeMismatch, "sort requires all numbers or all strings, got {}",
                                   type_name(list[0]));
                }
                for (usize index = 1; index < list.size(); ++index) {
                    if (sort_domain(list[index]) != domain) {
                        return vm.fail(ErrorCode::TypeMismatch,
                                       "sort requires all numbers or all strings, got {} and {}", type_name(list[0]),
                                       type_name(list[index]));
                    }
                }
            }
            std::ranges::stable_sort(list, value_less);
            slots[0] = Value::nil_val();
            return true;
        }

        // reverse() -> nil:就地整段反转。
        bool reverse_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "reverse expects no arguments, got {}", argc);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            std::ranges::reverse(list); // trivial 交换,无 GC 点
            slots[0] = Value::nil_val();
            return true;
        }

        // find(x) -> 整数或 nil:首个 == 元素的下标,未命中 nil(下标永不为 nil 故无歧义,
        // Ruby Array#index 同款 --aria 有负下标,-1 是合法下标,miss 时 xs[find(x)] 会静默
        // 取末元素)。判定收口 AriaArray::find,value_equal 无分配。
        bool find_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "find expects 1 argument, got {}", argc);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto  hit  = list.find(slots[1]);
            slots[0]         = hit ? Value::from_int(static_cast<i64>(*hit)) : Value::nil_val();
            return true;
        }

        // contains(x) -> Bool:成员判定收口 AriaArray::contains(list 无 in 表达式算子,
        // 本方法即成员测试口)。
        bool contains_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "contains expects 1 argument, got {}", argc);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_bool(list.contains(slots[1]));
            return true;
        }

        // size() -> 整数:元素数(全局 len(xs) 的方法形态)。
        bool size_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "size expects no arguments, got {}", argc);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_int(static_cast<i64>(list.size()));
            return true;
        }

        // is_empty() -> Bool:空表判定(len == 0 的谓词形;has_next 式动词前缀,empty 动词义
        // 会与 clear 混淆)。
        bool is_empty_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "is_empty expects no arguments, got {}", argc);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_bool(list.empty());
            return true;
        }

        // join(sep) -> string:元素经 format_value(显示形,嵌套字符串不带引号)转字符串后
        // 以 sep 连接(JS 式宽松,任意元素;空 list 返空串;sep 可为空串 --"ab" 式粘合)。
        // 底座 util::join(HashTable 迭代器批铺的缝在此兑现)。GC 走查:util::join 遍历
        // format_value 均无 GC 分配,唯一分配点 new_string 时 receiver 在 slots[0] 未覆写、
        // sep 在 slots[1] 经栈根。
        bool join_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.fail(ErrorCode::WrongArity, "join expects 1 argument, got {}", argc);
            }
            const auto sep = try_obj<ObjString>(slots[1]);
            if (sep == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "join separator must be a string, got {}", type_name(slots[1]));
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto  str  = util::join(list, sep->view(), format_value);
            slots[0]         = Value::from_obj(new_string(vm.gc(), str));
            return true;
        }

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

        // list 方法表:注册进 List bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        constexpr builtins::BuiltinEntry kListBuiltins[] = {
                {"push", push_fn},           {"pop", pop_fn},
                {"insert", insert_fn},       {"remove", remove_fn},
                {"remove_at", remove_at_fn}, {"clear", clear_fn},
                {"sort", sort_fn},           {"reverse", reverse_fn},
                {"find", find_fn},           {"contains", contains_fn},
                {"size", size_fn},           {"is_empty", is_empty_fn},
                {"join", join_fn},           {"iter", iter_fn},
        };

    } // namespace

    void register_list_builtins(GC& gc, ObjClass* klass) { register_builtin_methods(gc, klass, kListBuiltins); }

} // namespace aria
