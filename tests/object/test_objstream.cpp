#include <gtest/gtest.h>

#include <cstdio>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjStream.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/source_file.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::Builtin;
using aria::BuiltinFnEntry;
using aria::NativeCallInfo;
using aria::new_class;
using aria::new_module;
using aria::new_string;
using aria::ObjStream;
using aria::SourceFile;
using aria::String;
using aria::Value;

namespace {

    bool fn_is_stdout(const NativeCallInfo& info) {
        const auto stream = info.receiver<ObjStream>();
        if (stream == nullptr) {
            return false;
        }
        info.set_return_val(Value::from_bool(stream->stream() == stdout));
        return true;
    }

    constexpr BuiltinFnEntry kMethods[] = {{"is_stdout", fn_is_stdout}};

} // namespace

TEST(ObjStream, KeepsClassAndBindsNativeMethod) {
    AriaVM host;
    auto&  gc = host.gc();
    gc.set_stress(true);

    const auto module       = new_module(gc, "<script>", "");
    const auto module_guard = gc.make_guard(module);
    ObjStream* stream;
    {
        const auto klass = new_class(gc, "TestStream", host.object_class());
        auto       guard = gc.make_guard(klass);
        Builtin::register_class_methods(gc, klass, kMethods);
        const auto name = new_string(gc, "output");
        guard.push(name);
        stream = gc.new_object<ObjStream>(klass, stdout);
        module->globals().set(Value::from_obj(name), Value::from_obj(stream));
    }

    // 类先只通过流可达，回收后再导出它供脚本检查真实归属。
    gc.collect();
    const auto class_name = new_string(gc, "TestStream");
    module->globals().set(Value::from_obj(class_name), Value::from_obj(stream->klass()));

    SourceFile source{String{"<script>"}, String{"<script>"}, String{R"(
        assert(type(output) == "Stream");
        assert(is_a(output, TestStream));
        assert(is_a(output, Object));
        assert(output.is_stdout());
        var check = output.is_stdout;
        assert(check());
    )"}};
    const auto result = host.run(source, module);
    ASSERT_TRUE(result.has_value()) << result.error().message();
}
