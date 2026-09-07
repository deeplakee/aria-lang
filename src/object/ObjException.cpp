#include "object/ObjException.hpp"

#include "error/Error.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjException::ObjException(const ErrorCode code, ObjString* message) :
        Object{ObjType::EXCEPTION}, code_{code}, message_{message} {
        // message_ 恒非空:无消息的错误以空串 intern 兜底(工厂 new_string 常驻,无合法指针空态),
        // 与 ObjModule 的 name_/dir_ 同模式。构造期拦截非法 null。
        ASSERT(message != nullptr, "ObjException message must not be null");
    }

    Error ObjException::to_error() const {
        // message_ 已是完整烘焙串(含位置前缀与 "Category: Name" 前缀,与 Error::message()
        // 同形) -- 经 Error::from_baked **原样**回传,跳过 make_message 重烘
        // (经 from_detail 会把前缀再烘一遍成双重前缀),边界文案与 Error 直构逐字一致。
        return Error::from_baked(code_, message_->view());
    }

    void ObjException::trace(GC& gc) const noexcept {
        // message_ 恒非空(ctor ASSERT);mark_object 容 nullptr 仅防御。
        gc.mark_object(message_);
    }

    String ObjException::to_string() const {
        // 渲染完整烘焙消息(无引号):M3 catch 的 print(e)/str(e) 与 CLI 未捕获错误同款文案
        // (含位置前缀)。
        return String{message_->view()};
    }

    ObjException* new_exception(GC& gc, const ErrorCode code, const StringView message) {
        // message 为调用方烘好的完整消息串(原样存,不经 make_message -- 双重前缀由调用方
        // 传已烘焙串规避)。msg 是本函数内部新建(intern),自行守卫跨下方 new_object 顶
        // maybe_collect(工厂守「自己创建的」,同 new_module(StringView...) 模式)。new_string
        // 内部对 intern_insert 不加守卫(trivial 分配,见 ObjString 参考),此处守卫承重的是
        // new_object -- 真 GC 触发点。
        const auto msg   = new_string(gc, message);
        const auto guard = gc.make_guard(msg);
        return gc.new_object<ObjException>(code, msg);
    }

} // namespace aria
