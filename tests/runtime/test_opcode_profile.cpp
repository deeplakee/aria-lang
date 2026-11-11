#include <gtest/gtest.h>

#include <cstdlib>

#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "sys.hpp"
#include "util/source_file.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace {
    using namespace aria;

    constexpr auto       kStatsFlag  = "ARIA_OPCODE_STATS";
    constexpr StringView kMethodName = "unique_method_name_for_profile";
    constexpr StringView kNativeName = "unique_native_name_for_profile";

    i32 set_stats_flag(const char* value) {
#ifdef SYS_WINDOWS
        return _putenv_s(kStatsFlag, value ? value : "");
#else
        return value ? setenv(kStatsFlag, value, 1) : unsetenv(kStatsFlag);
#endif
    }

    void run_profiled_calls(AriaVM& vm) {
        auto&      gc     = vm.gc();
        const auto module = new_module(gc, "<profile-test>");
        auto       guard  = gc.make_guard(module);
        const auto key    = new_string(gc, "probe");
        guard.push(key);
        const auto function = new_function(gc, kNativeName, [](const NativeCallInfo& info) {
            info.set_return_val(Value::from_int(42));
            return true;
        });
        module->globals().set(Value::from_obj(key), Value::from_obj(function));

        src::SourceFile source{"<profile-test>", "<profile-test>.aria", R"(
            def Box { unique_method_name_for_profile() { return 42; } }
            assert(Box().unique_method_name_for_profile() == 42);
            assert(probe() == 42);
        )"};
        const auto      result = vm.run(source, module);
        ASSERT_TRUE(result.has_value()) << result.error().message();
    }

    TEST(OpcodeProfiler, PreservesNamesAcrossCollections) {
        const auto flag          = std::getenv(kStatsFlag);
        const auto previous_flag = flag ? Opt<String>{flag} : std::nullopt;
        ASSERT_EQ(set_stats_flag("1"), 0);

        testing::internal::CaptureStderr();
        {
            AriaVM vm;
            auto&  gc = vm.gc();
            gc.set_stress(true);
            auto guard = gc.make_guard();
            for (const StringView replacement: {"profile_first_replacement", "profile_second_replacement"}) {
                run_profiled_calls(vm);
                gc.collect();
                EXPECT_EQ(gc.intern_find(kMethodName, util::hash_str(kMethodName)), nullptr);
                EXPECT_EQ(gc.intern_find(kNativeName, util::hash_str(kNativeName)), nullptr);
                // 让其他字符串有机会复用空出的对象地址。
                guard.push(new_string(gc, replacement));
            }
        }
        const auto output = testing::internal::GetCapturedStderr();
        EXPECT_EQ(set_stats_flag(previous_flag ? previous_flag->c_str() : nullptr), 0);
        EXPECT_TRUE(output.contains("[opprofile] mcall 2 Instance unique_method_name_for_profile")) << output;
        EXPECT_TRUE(output.contains("[opprofile] ncall 2 Function unique_native_name_for_profile")) << output;
    }

} // namespace
