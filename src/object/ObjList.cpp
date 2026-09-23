#include "object/ObjList.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/EqualGuard.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "object/PrintGuard.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjList::ObjList(GC& gc) : Object{ObjType::LIST}, elements_{&gc} {}

    void ObjList::trace(GC& gc) const noexcept {
        elements_.trace(gc); // 遍历元素 mark_value(nil/int/f64 无对象子节点)
    }

    bool ObjList::equals(const Object* other) const noexcept {
        if (this == other) {
            return true;
        }
        // 环闭合:同对重遇已在比较链上,视为相等(余归纳,正则树同构);互环否则无限互递归。
        if (EqualGuard::is_cycle(this, other)) {
            return true;
        }
        const auto list = try_as<ObjList>(other);
        if (list == nullptr || elements_.size() != list->elements_.size()) {
            return false;
        }
        const EqualGuard guard{this, other};
        // 逐元素 value_equal:嵌套 list 经各自 equals 递归;value_equal 无 GC 分配,GC-pure 契约保持。
        for (usize index = 0; index < elements_.size(); ++index) {
            if (!value_equal(elements_[index], list->elements_[index])) {
                return false;
            }
        }
        return true;
    }

    Opt<Value> ObjList::load_index(AriaVM& vm, const Value key) {
        // Range 键 = 切片(流程见 slice)。
        if (const auto range = try_obj<ObjRange>(key)) {
            return slice(vm, range);
        }
        // 整数键:非整数 TypeMismatch;负下标从尾计数、归一化后越界 IndexOutOfBounds
        //(文案报原始键值)。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 raw = key.as_int();
        if (const auto slot = util::resolve_index(raw, elements_.size())) {
            return elements_[*slot];
        }
        return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", raw);
    }

    bool ObjList::store_index(AriaVM& vm, const Value key, const Value value) {
        // 键检查同读;不自动增长(越界即报,追加走 push 方法);写已存槽恒成功。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "list index must be an integer, got {}", aria::type_name(key));
        }
        const i64 raw = key.as_int();
        if (const auto slot = util::resolve_index(raw, elements_.size())) {
            elements_[*slot] = value;
            return true;
        }
        return vm.fail(ErrorCode::IndexOutOfBounds, "list index {} out of range", raw);
    }

    Opt<Value> ObjList::slice(AriaVM& vm, const ObjRange* range) {
        // 切片段解析(有上界与无上界两形态统一)收口 resolve_slice_bounds:nullopt = 无法形成合法
        // 区间,唯一失败报错就地烘焙(静态文案不插端点值)。长度与方向的折算全在解析口。
        const auto segment = resolve_slice_bounds(range, elements_.size());
        if (!segment) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "slice index out of range");
        }
        // 指针用 data() + 起点:空段(含无上界空后缀)起点落在末元素之后,operator[] 的越界断言
        // 不容它(段空不 deref,copy_* 对空 src 直接早返回)。
        // GC 走查:receiver 与 range 经调用方值栈为根,new_list 顶部 maybe_collect 安全;段拷
        // trivial 不触 GC;新 list 白色由 run_load_index 写回原槽根化。
        const auto list   = new_list(vm.gc());
        const auto source = Span<const Value>{elements_.data() + segment->start, segment->count};
        if (segment->is_reversed) {
            list->elements().copy_reversed_from(source); // 升序源段,由 Array 反转追加
        } else {
            list->elements().copy_from(source);
        }
        return Value::from_obj(list);
    }

    Opt<Value> ObjList::load_field(AriaVM& vm, ObjString* name) {
        // 内置侧两步,与实例路径同构(先委托类协议查表、后自己绑定,同 ObjInstance::load_field
        // 形):VM 的 List bootstrap 类经 ObjClass::load_field 沿链读穿透,miss 类措辞 fail 随
        // 协议透传;命中即恒绑定 this --内置类表条目全为原生函数、恒为方法,判别无须戳(表
        // 契约由 register_list_builtins 唯一写入口维持)。GC 走查:new_bound_method 是唯一分配
        // 点 --receiver(this)经调用方 peek 在栈(栈即根)、klass 经 VM 寄存器组根、命中值本体
        // 经类链 field_ 表可达(本地 hit 仅是值拷贝);bound 白色建成由 run_load_field 写回原槽
        // 根化。无缓存,每次取方法现场物化。
        const auto hit = vm.list_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjList::load_field_unbound(AriaVM& vm, ObjString* name) {
        // 方法调用解析(PREPARE_METHOD):与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- **不铸 ObjBoundMethod** 正是本 override 存在的理由(load_field 那条读路径要绑定;方法调用
        // 若改走读路径,每取一次方法白铸一个 bound -- 迭代协议每迭代两次,见集合计划 §4.4);
        // 调用区槽 0 保持 receiver 原样,正是原生要的 this。查找纯查询无分配,故本体是 load_field
        // 结果的纯透传(miss 的 fail 装箱在 ObjClass::load_field 内就地完成,receiver 与 name 由调用
        // 方根化:VM 侧 receiver peek 在栈、name 经常量池)。
        return vm.list_class()->load_field(vm, name);
    }

    String ObjList::debug_repr() const {
        // 环防护:自引用/互环时本 list 已在渲染路径上,截断 "[...]"(先查后挂,顺序反了
        // 自身即命中);不截断则元素重遇无限递归栈溢出。
        if (PrintGuard::is_cycle(this)) {
            return "[...]";
        }
        const PrintGuard guard{this};
        // [1, "ab"] 式:元素走 format_value_debug(嵌套字符串带引号;嵌套 list 递归)。
        return "[" + util::join(elements_, ", ", format_value_debug) + "]";
    }

    ObjList* new_list(GC& gc) {
        // 工厂无入参对象可守;调用方建成即发布进根(见头注释)。
        return gc.new_object<ObjList>(gc);
    }

} // namespace aria
