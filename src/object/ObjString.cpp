#include "object/ObjString.hpp"

#include <format>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "util/util.hpp"

namespace aria {

    ObjString::ObjString(GC& gc, const StringView src) :
        Object{util::hash_str(src), ObjType::STRING}, gc_{&gc}, length_{src.size()} {
        if (is_long()) {
            long_chars_ = gc.allocate<char>(length_ + 1);
            std::memcpy(long_chars_, src.data(), length_);
            long_chars_[length_] = '\0';
        } else {
            std::memcpy(short_chars_, src.data(), length_);
            short_chars_[length_] = '\0';
        }
    }

    ObjString::~ObjString() {
        if (is_long()) {
            gc_->deallocate<char>(long_chars_, length_ + 1);
        }
    }

    StringView ObjString::view() const noexcept {
        const char* chars = is_long() ? long_chars_ : short_chars_;
        return StringView{chars, length_};
    }

    String ObjString::to_string() const { return std::format("{}", view()); }

    String ObjString::debug_repr() const {
        // 字面量形式:转义 + 双引号包裹。
        return std::format("\"{}\"", util::escape_string(view()));
    }

    bool ObjString::equals(const Object* other) const noexcept {
        if (this == other)
            return true; // intern 命中:同指针同内容
        if (!is<ObjString>(other))
            return false;
        return view() == as<ObjString>(other)->view();
    }

    Opt<Value> ObjString::load_index(AriaVM& vm, const Value key) {
        // 整数键 = 字节域(计划 D5,与 len 同域):产出单字节 1-char string;多字节序列
        // 中间字节取该字节自身(字节契约的自然结果,非完整字符)。非整数 TypeMismatch、
        // 越界/负数 IndexOutOfBounds(越界值就地拼进文案,同 list)。
        if (!key.is_int()) {
            return vm.fail(ErrorCode::TypeMismatch, "string index must be an integer, got {}", aria::type_name(key));
        }
        const i64 index = key.as_int();
        if (index < 0 || static_cast<u64>(index) >= length_) {
            return vm.fail(ErrorCode::IndexOutOfBounds, "string index {} out of range", index);
        }
        // GC 走查:receiver 经调用方值栈 peek 为根(「栈即根」),new_string 的 intern 查找
        // 与分配均在其后;新串白色建成由 run_load_index 写回原槽根化。
        return Value::from_obj(new_string(vm.gc(), view().substr(index, 1)));
    }

    bool ObjString::store_index(AriaVM& vm, const Value key, const Value value) {
        // string 不可变:下标写恒报错(定向文案)。key/value 未消费:先拒操作本身,键值
        // 检查无意义;签名由协议缝钉死(与 list/map 分支同形)。
        return vm.fail(ErrorCode::TypeMismatch, "string does not support subscript assignment");
    }

    Opt<Value> ObjString::load_field(AriaVM& vm, ObjString* name) {
        // 内置侧两步,与实例路径同构(先委托类协议查表、后自己绑定,同 ObjInstance::load_field
        // 形):VM 的 String bootstrap 类经 ObjClass::load_field 沿链读穿透,miss 类措辞 fail 随
        // 协议透传;命中即恒绑定 this --内置类表条目全为原生函数、恒为方法,判别无须戳(表
        // 契约由 register_string_methods 唯一写入口维持)。GC 走查:new_bound_method 是唯一分配
        // 点 --receiver(this)经调用方 peek 在栈(栈即根)、klass 经 VM 寄存器组根、命中值本体
        // 经类链 field_ 表可达(本地 hit 仅是值拷贝);bound 白色建成由 run_load_field 写回原槽
        // 根化。内置侧无 fields 缓存,每次取方法现场物化。
        const auto hit = vm.string_class()->load_field(vm, name);
        if (!hit) {
            return std::nullopt; // 已 fail(契约透传)
        }
        return Value::from_obj(new_bound_method(vm.gc(), *hit, Value::from_obj(this)));
    }

    ObjString* new_string(GC& gc, const StringView src) {
        if (const auto found = gc.intern_find(src)) {
            return found; // 命中驻留池:返回已有串,不分配、不 GC
        }
        // s 此刻白色无根,但 intern_insert -> InternPool::insert -> allocate<ObjString*> 走 trivial
        // 分配(不触发 GC,见 GC.hpp 核心不变式),故 s 跨 insert 不会被回收,无需守卫。
        const auto s = gc.new_object<ObjString>(gc, src); // 顶部 maybe_collect 在 s 诞生前完成
        gc.intern_insert(s);
        return s;
    }

} // namespace aria
