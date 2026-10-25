// Native control integration: hidden windows only, no API/config/notifications.
#include "../src/CodexTaskbar.cpp"

static int failures = 0;
static int checks = 0;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
        ++failures; \
    } \
} while (0)

static std::wstring ReadTooltipText() {
    wchar_t text[1024]{};
    TOOLINFOW tool{};
    tool.cbSize = sizeof(tool);
    tool.hwnd = g_widget;
    tool.uId = (UINT_PTR)g_widget;
    tool.lpszText = text;
    SendMessageW(g_widgetTooltip, TTM_GETTEXTW, _countof(text), (LPARAM)&tool);
    return text;
}

int main() {
    g_inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.hInstance = g_inst;
    wc.lpfnWndProc = WidgetProc;
    wc.lpszClassName = kWidgetClass;
    CHECK(RegisterClassW(&wc) != 0);
    g_anchor = CreateWindowExW(0, L"STATIC", L"Tooltip test owner", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    CHECK(g_anchor != nullptr);
    g_usage.ok = true;
    g_usage.planType = L"prolite";
    g_usage.weeklyWindow = {true, 12, (long long)time(nullptr) + 604800, 604800};
    CreateWidget();
    CHECK(g_widget != nullptr && g_widgetTooltip != nullptr);
    if (g_widgetTooltip) {
        CHECK(SendMessageW(g_widgetTooltip, TTM_GETTOOLCOUNT, 0, 0) == 1);
        CHECK(ReadTooltipText().find(L"Codex | Plan: Pro Lite") == 0);
        CHECK(ReadTooltipText().find(L"wk: 88% left, reset in 7d 0h") != std::wstring::npos);
        CHECK(ReadTooltipText().find(L"Reset (local): ") != std::wstring::npos);
        g_cfg.displayMode = 1;
        CHECK(ReadTooltipText().find(L"wk: 12% used") != std::wstring::npos);
        // The same tool follows fresh data instead of caching the first callback.
        g_usage.planType = L"plus";
        CHECK(ReadTooltipText().find(L"Plan: Plus") != std::wstring::npos);
        g_usage.planType.clear();
        CHECK(ReadTooltipText().find(L"Plan: Unknown") != std::wstring::npos);
    }
    HWND oldTooltip = g_widgetTooltip;
    DestroyWidget();
    CHECK(!g_widget && !g_widgetTooltip && !IsWindow(oldTooltip));
    CreateWidget();
    CHECK(g_widget && g_widgetTooltip);
    CHECK(ReadTooltipText().find(L"Plan: Unknown") != std::wstring::npos);
    oldTooltip = g_widgetTooltip;
    // Destruction initiated by the host must also clean up the owned tooltip.
    DestroyWindow(g_widget);
    CHECK(!g_widgetTooltip && !IsWindow(oldTooltip));
    g_widget = nullptr;
    HWND about = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_ABOUT), g_anchor, AboutProc, 0);
    CHECK(about != nullptr);
    if (about) {
        wchar_t version[80]{};
        GetDlgItemTextW(about, IDC_ABOUT_VERSION, version, _countof(version));
        CHECK(std::wstring(version) == L"Version " + U82W(APP_VERSION_TEXT));
        CHECK(GetDlgItem(about, IDC_RELEASES) != nullptr);
        CHECK(GetDlgItem(about, IDOK) != nullptr);
        DestroyWindow(about);
    }
    DestroyWindow(g_anchor);
    g_anchor = nullptr;
    std::printf("Tooltip controls: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
