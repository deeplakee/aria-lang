#include "runtime/builtins/ListClass.hpp"

#include <algorithm>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // list 方法实现(NativeFn 方法调用形态见 Builtin.hpp)惯例:receiver 解开后直取
        // elements() 绑为 list(仅 iter 需要 ObjList* 本体传给迭代器);段搬移类变更(insert/
        // remove_at)收口 Array 原语,方法体只余域检查与调用。

        // push(x) -> nil:追加 x 到末尾(任意 Value);返回 nil。
        bool fn_push(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            // 绑定路径契约:slots[0] 恒本 list(仅经 load_field 绑定触达),DEBUG 下 as 走
            // dynamic_cast 校验。
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            list.push(slots[1]); // trivial 分配不触 GC
            slots[0] = Value::nil_val();
            return true;
        }

        // pop() -> 末元素:移除并返回末元素(任意 Value);空表报 IndexOutOfBounds(fail-fast,
        // nil 哨兵不可行 --list 可合法存 nil)。
        bool fn_pop(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            if (list.empty()) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "pop from empty list");
            }
            slots[0] = list[list.size() - 1];
            list.pop(); // 搬移时 receiver 已离 slots[0],trivial 无 GC 点
            return true;
        }

        // insert(i, x) -> nil:在位置 i 之前插入 x(任意 Value)。负数从尾计数、指「该下标元素
        // 之前」,合法域 [-(size), size] 收口 util::resolve_position(上界放宽到追加位)。段右移
        // 腾位收口 Array::insert。
        bool fn_insert(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.arity_error(argc, 2);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "insert index must be an integer, got {}", type_name(slots[1]));
            }
            auto&      list     = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto position = util::resolve_position(slots[1].as_int(), list.size());
            if (!position) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "insert index {} out of range", slots[1].as_int());
            }
            list.insert(*position, slots[2]);
            slots[0] = Value::nil_val();
            return true;
        }

        // remove(x) -> Bool:移除**全部** == 命中元素;命中 true、未命中 false 不报错 --miss 走
        // 返回值,与 find 返 -1 / contains 返 false 同族。收口 AriaArray::remove(一趟稳定压缩)。
        bool fn_remove(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]   = Value::from_bool(list.remove(slots[1]));
            return true;
        }

        // remove_at(i) -> i 处元素:按位置移除并返回(负数从尾计数、与下标读写同语义,越界报
        // 原始键值)。段左移补位收口 Array::remove_at。
        bool fn_remove_at(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "remove_at index must be an integer, got {}",
                               type_name(slots[1]));
            }
            auto&      list = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto slot = util::resolve_index(slots[1].as_int(), list.size());
            if (!slot) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "remove_at index {} out of range", slots[1].as_int());
            }
            slots[0] = list[*slot];
            list.remove_at(*slot);
            return true;
        }

        // clear() -> nil:清空(长度归零,容量保留)。重绑 xs = [] 换新表别名仍见旧内容,本方法
        // 供共享可变状态原地清空。
        bool fn_clear(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            list.clear();
            slots[0] = Value::nil_val();
            return true;
        }

        // 排序域:可比较域两支(数值 / 字符串)加域外单列(NotComparable 覆盖 nil/bool/容器等)。
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

        // sort() -> nil:就地升序(变更方法返 nil)。域 = 全数值或全字符串,先整体域检再排序 --
        // 比较器免「不可比」分支,排序中途无失败路径;升序判定收口 value_less(value 层自然序)。
        // GC 走查:域检与比较均无 GC 分配;stable_sort 临时缓冲走 std 内存(非 GC 堆),receiver 在
        // slots[0] 未覆写。稳定序:等值元素(如 int 1 与 f64 1.0)保输入相对序。
        bool fn_sort(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
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
        bool fn_reverse(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            std::ranges::reverse(list); // trivial 交换,无 GC 点
            slots[0] = Value::nil_val();
            return true;
        }

        // find(x) -> 整数或 nil:首个 == 元素的下标,未命中 nil(下标永不为 nil 故无歧义,miss 时
        // xs[find(x)] 会静默取末元素)。判定收口 AriaArray::find,value_equal 无分配。
        bool fn_find(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            const auto  hit  = list.find(slots[1]);
            slots[0]         = hit ? Value::from_int(static_cast<i64>(*hit)) : Value::nil_val();
            return true;
        }

        // contains(x) -> Bool:成员判定收口 AriaArray::contains(list 无 in 表达式算子,本方法即
        // 成员测试口)。
        bool fn_contains(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_bool(list.contains(slots[1]));
            return true;
        }

        // size() -> 整数:元素数。
        bool fn_size(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_int(static_cast<i64>(list.size()));
            return true;
        }

        // is_empty() -> Bool:空表判定(len == 0 的谓词形)。
        bool fn_is_empty(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& list = Object::as<ObjList>(slots[0].as_obj())->elements();
            slots[0]         = Value::from_bool(list.empty());
            return true;
        }

        // join(sep) -> string:元素经 format_value 转字符串后以 sep 连接(任意元素;空 list 返空串;
        // sep 可为空串)。底座 util::join。GC 走查:util::join 遍历 format_value 均无 GC 分配,唯一
        // 分配点 new_string 时 receiver 在 slots[0] 未覆写、sep 在 slots[1] 经栈根。
        bool fn_join(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
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

        // iter() -> 迭代器:list 与其迭代器成对(铸造口按类型解开 receiver)。GC 约束:list 在
        // slots[0] 于栈根,迭代器白色建成**先写回槽发布再返回**,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto list = Object::as<ObjList>(slots[0].as_obj());
            slots[0]        = Value::from_obj(new_list_iterator(vm.gc(), list));
            return true;
        }

        // 运算符重载方法(函数名与 runtime/string_constant.hpp 的 StringConstant 一一对应,经 AriaVM::run_binary_operator
        // 取用;也是"算子 = 方法"的唯一实现处)。名字与失败文案都是**就地字面量**(与方法名同形):
        // 文案打方法名,与注册键同处一文件、golden 钉住拼写。list 只有 `+` 与 `*`(乘数严格 int,
        // f64 一律拒 -- 同下标访问口径;负数报错不静默得空,与负数下标报错约定一致);乘除模、
        // 比较不重载(判等与下标本就不参与重载)。

        // __add__ -> 新 list:两表拼接。浅拷:元素 Value 逐位复制,嵌套容器两表共享同一对象
        //(与下标读同口径,深拷需逐元素另建)。GC 走查:new_list 顶部 maybe_collect 时两侧实参
        // 经调用区槽在栈(receiver 占 slots[0],「栈即根」);此后 reserve/copy_from 全程 trivial
        // 无 GC 点,建成即写回槽 0 发布。
        bool fn___add__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_obj<ObjList>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__add__ requires two lists, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const auto lhs = Object::as<ObjList>(slots[0].as_obj());
            const auto out = new_list(vm.gc());
            out->elements().reserve(lhs->elements().size() + rhs->elements().size());
            out->elements().copy_from(lhs->elements());
            out->elements().copy_from(rhs->elements());
            slots[0] = Value::from_obj(out);
            return true;
        }

        // __mul__ -> 新 list:整次重复(count 次接尾追加自身元素,0 次得空表;浅拷同 __add__)。
        // GC 走查同 __add__:唯一分配点 new_list,其后 reserve + 逐轮 copy_from 全程 trivial。
        bool fn___mul__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a list and an integer, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const i64 count = slots[1].as_int();
            if (count < 0) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a non-negative integer, got {}", count);
            }
            const auto lhs = Object::as<ObjList>(slots[0].as_obj());
            const auto out = new_list(vm.gc());
            out->elements().reserve(lhs->elements().size() * static_cast<usize>(count));
            for (i64 i = 0; i < count; ++i) {
                out->elements().copy_from(lhs->elements());
            }
            slots[0] = Value::from_obj(out);
            return true;
        }

        // list 方法表:注册进 List bootstrap 类(注册机制见 runtime/builtins/Builtin.hpp)。
        constexpr BuiltinFnEntry kListBuiltins[] = {
                {"push", fn_push},
                {"pop", fn_pop},
                {"insert", fn_insert},
                {"remove", fn_remove},
                {"remove_at", fn_remove_at},
                {"clear", fn_clear},
                {"sort", fn_sort},
                {"reverse", fn_reverse},
                {"find", fn_find},
                {"contains", fn_contains},
                {"size", fn_size},
                {"is_empty", fn_is_empty},
                {"join", fn_join},
                {"iter", fn_iter},
                // 运算符重载方法(list 只有 `+` 与 `*`;键与函数名对应的钩子名同形,漏改其一时
                // ListClass::kOperatorFns 清单按名查不到、bootstrap 断言即报)
                {"__add__", fn___add__},
                {"__mul__", fn___mul__},
        };

    } // namespace

    void ListClass::register_methods(GC& gc, ObjClass* klass) { Builtin::register_class_methods(gc, klass, kListBuiltins); }

} // namespace aria
