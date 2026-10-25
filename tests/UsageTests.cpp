// Exercise the production parser and display logic without network, credentials,
// config writes, windows, or notifications. No separate test framework is needed.
#include "../src/CodexTaskbar.cpp"

static int checks = 0;
static int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static JVal Parse(const char* json) {
    JVal v;
    JParser parser(json, json + std::strlen(json));
    CHECK(parser.Parse(v));
    return v;
}

static Usage ApiUsage(const char* json) {
    JVal v = Parse(json);
    Usage u;
    u.ok = ReadApiUsage(v, u);
    return u;
}

static void TestPlusAndSwappedWindows() {
    Usage plus = ApiUsage(R"({"plan_type":"plus","rate_limit":{
        "primary_window":{"used_percent":17,"reset_at":2000018000,"limit_window_seconds":18000},
        "secondary_window":{"used_percent":28,"reset_at":2000604800,"limit_window_seconds":604800}
    }})");
    CHECK(plus.ok);
    CHECK(plus.planType == L"plus");
    CHECK(plus.shortWindow.valid && plus.shortWindow.used == 17);
    CHECK(plus.weeklyWindow.valid && plus.weeklyWindow.used == 28);
    CHECK(MainWindow(plus).used == 17);
    CHECK(ShowWeeklyRow(plus, true));
    CHECK(!ShowWeeklyRow(plus, false));
    CHECK(WindowLabel(plus.shortWindow) == L"5h");
    CHECK(WindowLabel(plus.weeklyWindow) == L"wk");
    CHECK(FormatRemaining(EvalLimit(plus.shortWindow, 2000000000).resetInSec, false) == L"5h 00m");

    Usage swapped = ApiUsage(R"({"rate_limit":{
        "primary_window":{"used_percent":28,"reset_at":2000604800,"limit_window_seconds":604800},
        "secondary_window":{"used_percent":17,"reset_at":2000018000,"limit_window_seconds":18000}
    }})");
    CHECK(swapped.shortWindow.used == plus.shortWindow.used);
    CHECK(swapped.shortWindow.resetAt == plus.shortWindow.resetAt);
    CHECK(swapped.weeklyWindow.used == plus.weeklyWindow.used);
    CHECK(swapped.weeklyWindow.resetAt == plus.weeklyWindow.resetAt);
    CHECK(ShowWeeklyRow(swapped, true));
}

static void TestWeeklyOnly() {
    Usage u = ApiUsage(R"({"plan_type":"prolite","rate_limit":{
        "primary_window":{"used_percent":11,"reset_at":2000532800,"limit_window_seconds":604800},
        "secondary_window":null
    }})");
    CHECK(u.ok && !u.shortWindow.valid && u.weeklyWindow.valid);
    CHECK(u.planType == L"prolite");
    CHECK(MainWindow(u).used == 11);
    CHECK(MainWindow(u).windowSec == 604800);
    CHECK(WindowLabel(MainWindow(u)) == L"wk");
    CHECK(IsLongWindow(MainWindow(u)));
    CHECK(!ShowWeeklyRow(u, true));
    CHECK(!ShowWeeklyRow(u, false));
    LimitState state = EvalLimit(MainWindow(u), 2000000000);
    CHECK(PercentText(100 - state.usedPct) == L"89%");
    CHECK(PercentText(state.usedPct) == L"11%");
    CHECK(FormatRemaining(state.resetInSec, IsLongWindow(MainWindow(u))) == L"6d 4h");

    Usage secondaryOnly = ApiUsage(R"({"rate_limit":{
        "primary_window":null,
        "secondary_window":{"used_percent":11,"reset_at":2000532800,"limit_window_seconds":604800}
    }})");
    CHECK(!secondaryOnly.shortWindow.valid && secondaryOnly.weeklyWindow.valid);
    CHECK(WindowLabel(MainWindow(secondaryOnly)) == L"wk");
    CHECK(MainWindow(secondaryOnly).used == MainWindow(u).used);

    u.weeklyWindow.used = 100;
    CHECK(EvalLimit(MainWindow(u), 2000532799).exhausted);
    CHECK(TrayResetMask(u, 2000532799) == 0);
    state = EvalLimit(MainWindow(u), 2000532800);
    CHECK(state.resetPending && !state.exhausted);
    CHECK(TrayResetMask(u, 2000532800) == 2);
    // Even if the short window was already pending, a weekly reset must update the tray.
    u.shortWindow = { true, 100, 2000532700, 18000 };
    CHECK(TrayResetMask(u, 2000532799) == 1);
    CHECK(TrayResetMask(u, 2000532800) == 3);
}

static void TestRolloutSchemas() {
    Usage u;
    CHECK(ReadRolloutUsage(Parse(R"({"timestamp":2000000000,"payload":{"rate_limits":{
        "limit_id":"codex","plan_type":"prolite",
        "primary":{"used_percent":11,"window_minutes":10080,"resets_at":2000532800},
        "secondary":null
    }}})"), u));
    CHECK(u.ok && u.stale && u.sampleAt == 2000000000);
    CHECK(u.source == L"Codex session log");
    CHECK(u.planType == L"prolite");
    CHECK(!u.shortWindow.valid && u.weeklyWindow.valid);
    CHECK(u.weeklyWindow.windowSec == 604800 && u.weeklyWindow.resetAt == 2000532800);
    CHECK(WindowLabel(MainWindow(u)) == L"wk");

    CHECK(ReadRolloutUsage(Parse(R"({"payload":{"rate_limits":{
        "primary":{"used_percent":17,"window_minutes":300,"resets_at":2000018000},
        "secondary":{"used_percent":28,"window_minutes":10080,"resets_at":2000604800}
    }}})"), u));
    CHECK(u.shortWindow.valid && u.shortWindow.windowSec == 18000 && u.shortWindow.used == 17);
    CHECK(u.weeklyWindow.valid && u.weeklyWindow.windowSec == 604800 && u.weeklyWindow.used == 28);
    CHECK(ShowWeeklyRow(u, true));
    CHECK(u.planType.empty());  // do not carry an old tariff across snapshots

    CHECK(ReadRolloutUsage(Parse(R"({"payload":{"rate_limits":{
        "primary":{"used_percent":28,"window_minutes":10080,"reset_at":2000604800},
        "secondary":{"used_percent":17,"window_minutes":300,"reset_at":2000018000}
    }}})"), u));
    CHECK(u.shortWindow.windowSec == 18000 && u.shortWindow.used == 17);
    CHECK(u.weeklyWindow.windowSec == 604800 && u.weeklyWindow.used == 28);
    CHECK(!ReadRolloutUsage(Parse(R"({"payload":{"rate_limits":{
        "limit_id":"codex_spark",
        "primary":{"used_percent":99,"window_minutes":300,"resets_at":2000018000}
    }}})"), u));
    CHECK(u.shortWindow.used == 17);  // ignored buckets must not replace Codex data
}

static void TestMissingAndLegacyWindows() {
    Usage u = ApiUsage(R"({"rate_limit":{
        "primary_window":{"used_percent":3,"reset_at":2000018000}
    }})");
    CHECK(u.shortWindow.valid && u.shortWindow.windowSec == 18000);
    CHECK(!u.weeklyWindow.valid && !ShowWeeklyRow(u, true));
    CHECK(WindowLabel(MainWindow(u)) == L"5h");
    ReadUsageWindows(Parse(R"({
        "secondary_window":{"used_percent":42,"reset_at":2000604800,"limit_window_seconds":0}
    })"), u, false);
    CHECK(!u.shortWindow.valid && u.weeklyWindow.valid);  // no previous-plan residue
    CHECK(u.weeklyWindow.windowSec == 604800);
    ReadUsageWindows(Parse(R"({
        "primary_window":{"used_percent":42,"limit_window_seconds":604800},
        "secondary_window":null
    })"), u, false);
    CHECK(!u.shortWindow.valid && !u.weeklyWindow.valid);
    ReadUsageWindows(Parse(R"({"primary_window":null,"secondary_window":null})"), u, false);
    CHECK(!MainWindow(u).valid);
    CHECK(TrayResetMask(u, 2000000000) == 0);
    CHECK(!ReadRolloutUsage(Parse(R"({"payload":{"rate_limits":null}})"), u));

    WndInfo freshWeek{ true, 0, 2000604802, 604800 };
    CHECK(EvalLimit(freshWeek, 2000000000).resetInSec == 604800);
    CHECK(FormatRemaining(EvalLimit(freshWeek, 2000000000).resetInSec, true) == L"7d 0h");
}

static void TestPlanTooltips() {
    Usage u = ApiUsage(R"({"plan_type":"prolite","rate_limit":{
        "primary_window":{"used_percent":12,"reset_at":2000532800,"limit_window_seconds":604800}
    }})");
    CHECK(UsageTooltip(u, 2000000000, true) == L"Codex | Plan: Pro Lite\nwk: 88% left, reset 6d 4h");
    CHECK(UsageTooltip(u, 2000000000, false) == L"Codex | Plan: Pro Lite\nwk: 12% used, reset 6d 4h");
    CHECK(UsageTooltip(u, 2000532800, true).find(L"wk: ~100% left, resetting") != std::wstring::npos);
    CHECK(PlanName(L"plus") == L"Plus");
    CHECK(PlanName(L"pro") == L"Pro");
    CHECK(PlanName(L"business") == L"Business");
    CHECK(PlanName(L"enterprise") == L"Enterprise");
    CHECK(PlanName(L"new_plan") == L"new_plan");
    CHECK(ReadPlanType(Parse(R"({"plan_type":null})")).empty());
    CHECK(ReadPlanType(Parse(R"({"plan_type":123})")).empty());
    CHECK(ReadPlanType(Parse(R"({"plan_type":"plus\n\t"})")) == L"plus");
    u.planType.clear();
    CHECK(UsageTooltip(u, 2000000000, true).find(L"Plan: Unknown") != std::wstring::npos);
    u.planType = L"enterprise";
    u.shortWindow = {true, 100, 2000000010, 18000};
    u.stale = true;
    std::wstring tip = UsageTooltip(u, 2000000000, true);
    CHECK(tip.find(L"Session log (age unknown)") != std::wstring::npos);
    CHECK(tip.find(L"5h: 0% left") != std::wstring::npos);
    CHECK(tip.find(L"wk: 88% left") != std::wstring::npos);
    CHECK(tip.size() <= 127);
    u.planType = L"a_future_plan_with_a_very_long_identifier";
    u.sampleAt = 1999395200;
    tip = UsageTooltip(u, 2000000000, true);
    CHECK(tip.find(L"Session log (7d 0h old)") != std::wstring::npos);
    CHECK(tip.size() <= 127);
    u.ok = false;
    u.stale = false;
    u.error = L"No network";
    CHECK(UsageTooltip(u, 2000000000, true).find(L"No network") != std::wstring::npos);
}

int main() {
    TestPlusAndSwappedWindows();
    TestWeeklyOnly();
    TestRolloutSchemas();
    TestMissingAndLegacyWindows();
    TestPlanTooltips();
    std::printf("Usage tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
