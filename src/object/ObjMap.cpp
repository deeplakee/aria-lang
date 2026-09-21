#include "object/ObjMap.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/EqualGuard.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/PrintGuard.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    ObjMap::ObjMap(GC& gc) : Object{ObjType::MAP}, table_{&gc} {}

    void ObjMap::trace(GC& gc) const noexcept {
        table_.trace(gc); // 遍历占用槽 mark_value key+value(nil/int/f64 无对象子节点)
    }

    bool ObjMap::equals(const Object* other) const noexcept {
        if (this == other) {
            return true;
        }
        // 环闭合:同对重遇已在比较链上,视为相等(余归纳,正则树同构);互环否则无限互递归。
        if (EqualGuard::is_cycle(this, other)) {
            return true;
        }
        const auto map = try_as<ObjMap>(other);
        if (map == nullptr || table_.size() != map->table_.size()) {
            return false;
        }
        const EqualGuard guard{this, other};
        // 逐键 range-for(首个 miss 即 return 退出):键命中按表内语义 ===(find 即
        // value_identical 匹配),值 value_equal(嵌套容器经各自 equals 递归);find/value_equal
        // 均无 GC 分配,GC-pure 契约保持。
        for (const auto& [key, value]: table_) {
            if (const auto entry = map->table_.find(key); entry == nullptr || !value_equal(value, entry->value)) {
                return false;
            }
        }
        return true;
    }

    Opt<Value> ObjMap::load_index(AriaVM& vm, const Value key) {
        // 任意键(不做 list 式键型检查);miss KeyError,键走 debug 形入文案(嵌套字符串带
        // 引号,避免裸串歧义;环防护由 debug_repr 的 PrintGuard 自理)。
        if (const auto entry = table_.find(key)) {
            return entry->value;
        }
        return vm.fail(ErrorCode::KeyError, "map key not found: {}", format_value_debug(key));
    }

    bool ObjMap::store_index(AriaVM& vm, const Value key, const Value value) {
        // 恒成功,命中覆写、未命中新增键(set 的两条路径均无报错)。vm 未消费:签名由协议
        // 缝钉死(与 list 分支同形),报错面为空是 map 写语义的本形。
        table_.set(key, value);
        return true;
    }

    Opt<Value> ObjMap::load_field(AriaVM& vm, ObjString* name) {
        // 内置侧两步,与实例路径同构(先委托类协议查表、后自己绑定,同 ObjInstance::load_field
        // 形):VM 的 Map bootstrap 类经 ObjClass::load_field 沿链读穿透,miss 类措辞 fail 随
        // 协议透传;命中即恒绑定 this --内置类表条目全为原生函数、恒为方法,判别无须戳(表
        // 契约由 register_map_builtins 唯一写入口维持)。GC 走查:new_bound_method 是唯一分配
        // 点 --receiver(this)经调用方 peek 在栈(栈即根)、klass 经 VM 寄存器组根、命中值本体
        // 经类链 field_ 表可达(本地 hit 仅是值拷贝);bound 白色建成由 run_load_field 写回原槽
        // 根化。内置侧无 fields 缓存,每次取方法现场物化。
        const auto hit = vm.map_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    Opt<Value> ObjMap::resolve_invoke(AriaVM& vm, ObjString* name) {
        // 方法调用解析(PREPARE_METHOD):与 load_field 同一趟类表查找,命中直取类表原生值交 VM 调用
        // -- **不铸 ObjBoundMethod** 正是本 override 存在的理由(load_field 那条读路径要绑定;内置侧
        // 无 fields 缓存可回填,两步形态每取一次方法白铸一个,迭代协议每迭代两次,见集合计划 §4.4);
        // 调用区槽 0 保持 receiver 原样,正是原生要的 this。查找纯查询无分配,故本体是 load_field
        // 结果的纯透传(miss 的 fail 装箱在 ObjClass::load_field 内就地完成,receiver 与 name 由调用
        // 方根化:VM 侧 receiver peek 在栈、name 经常量池)。
        return vm.map_class()->load_field(vm, name);
    }

    String ObjMap::debug_repr() const {
        // 环防护:自引用/互环时本 map 已在渲染路径上,截断 "{...}"(先查后挂,顺序反了
        // 自身即命中);不截断则键值重遇无限递归栈溢出。
        if (PrintGuard::is_cycle(this)) {
            return "{...}";
        }
        const PrintGuard guard{this};
        // {"a": 1} 式:键值均走 format_value_debug(嵌套字符串带引号;嵌套容器递归 debug_repr);
        // 渲染序随占用槽(unspecified,与迭代序同属计划 D4)。
        const auto entry_repr = [](const AriaHashTable::Entry& entry) {
            return format_value_debug(entry.key) + ": " + format_value_debug(entry.value);
        };
        return "{" + util::join(table_, ", ", entry_repr) + "}";
    }

    ObjMap* new_map(GC& gc) {
        // 工厂无入参对象可守;调用方建成即发布进根(见头注释)。
        return gc.new_object<ObjMap>(gc);
    }

} // namespace aria
