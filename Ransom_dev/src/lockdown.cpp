// ============================================================================
//  lockdown.cpp —— 见 lockdown.h 里的说明
// ============================================================================
#include "lockdown.h"

#include "entity_log.h"

#include <cwchar>
#include <string>
#include <vector>

namespace {

    // 守望间隔。太密会白烧 CPU（每次都 EnumWindows + GetWindowPlacement），
    // 太疏就会看见窗口「跳」一下。150ms 实测是这个平衡点：
    // 用户能察觉到的最短闪现在 100ms 上下，比它略长一点就行。
    const DWORD kRepressMs = 150;

    struct LockedWindow {
        HWND  hwnd   = nullptr;
        DWORD pid    = 0;
        std::wstring title;

        // 被系统/程序自己弹回来的次数。有些窗口（外壳的经验宿主、音量合成器
        // 这类跟着系统跑的界面）被最小化之后会立刻自己弹回来，再收再弹，
        // 守望就会变成每 150ms 一次的徒劳死循环 —— 白烧 CPU、任务栏还会闪。
        // 遇到这种就认输：试够 kGiveUpAfter 次之后放弃它，日志里记一笔。
        int   resist = 0;
    };

    // 放弃阈值。给 6 次是为了容忍「程序启动瞬间自己抢前台」这种一次性反抗，
    // 真正的顽固分子很快就会被排除掉。
    const int kGiveUpAfter = 6;

    // 一次守望里最多重新收起几个。防止某个窗口群集体造反时把这一刻卡住。
    const int kMaxPerTick = 8;

    std::vector<LockedWindow> g_locked;
    std::vector<std::wstring> g_ownClasses;

    bool  g_active    = false;
    bool  g_enabled   = true;
    bool  g_blockKeys = true;
    DWORD g_lastTick  = 0;
    int   g_repressed = 0;

    DWORD g_selfPid = 0;

    // ---------------------------------------------------------------- 工具

    bool IsOwnClass(const wchar_t* cls)
    {
        for (size_t i = 0; i < g_ownClasses.size(); ++i)
            if (_wcsicmp(g_ownClasses[i].c_str(), cls) == 0) return true;
        return false;
    }

    std::wstring WindowTitle(HWND h)
    {
        const int n = GetWindowTextLengthW(h);
        if (n <= 0) return std::wstring();
        std::wstring s((size_t)n + 1, L'\0');
        const int got = GetWindowTextW(h, &s[0], n + 1);
        s.resize(got > 0 ? (size_t)got : 0);
        return s;
    }

    DWORD WindowPid(HWND h)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        return pid;
    }

    // 这个窗口现在该不该被收走。
    //
    // 判定顺序是刻意的：先排除「结构上不该碰的」（子窗口、本进程、工具窗口、
    // 有主窗口的从属窗口），再看「程序自己允不允许最小化」。
    // 最后那条用 GetWindowPlacement 的 showCmd 判断，而不是自己拼
    // WS_MINIMIZEBOX / WS_SYSMENU —— 外壳（explorer）决定一个窗口能不能
    // 最小化，靠的就是这个字段，跟它保持一致才不会出现「任务栏按钮点了
    // 没反应」这种收了一半的窗口。
    bool ShouldMinimize(HWND h)
    {
        if (!IsWindowVisible(h)) return false;
        if (GetWindow(h, GW_OWNER) != nullptr) return false;      // 对话框之类，跟着主窗口走

        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return false;                  // 工具窗口、托盘、输入法

        wchar_t cls[128] = {0};
        if (!GetClassNameW(h, cls, 128)) return false;
        if (IsOwnClass(cls)) return false;

        if (WindowPid(h) == g_selfPid) return false;

        // 外壳判据：只有允许最小化的窗口才有任务栏按钮，也才值得收。
        WINDOWPLACEMENT wp;
        ZeroMemory(&wp, sizeof(wp));
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(h, &wp)) return false;
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_SHOWMINNOACTIVE)
            return false;                                         // 已经是收着的，不用管

        // 没有标题的多半是隐藏的运行时窗口、消息窗口，收它没有意义，
        // 而且某些程序会因此行为异常。
        return !WindowTitle(h).empty();
    }

    BOOL CALLBACK CollectProc(HWND h, LPARAM lp)
    {
        std::vector<HWND>* out = (std::vector<HWND>*)lp;
        if (ShouldMinimize(h)) out->push_back(h);
        return TRUE;
    }

    // 这一轮把所有该收的收走，并记下明细。
    int MinimizeEverything()
    {
        std::vector<HWND> targets;
        targets.reserve(64);
        EnumWindows(CollectProc, (LPARAM)&targets);

        int done = 0;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            HWND h = targets[i];
            const std::wstring title = WindowTitle(h);
            const DWORD pid = WindowPid(h);

            // 界面线程卡住时 ShowWindow 可能等一会儿，这里用异步版本，
            // 免得整个勒索开场被某个卡死的程序拖住。
            ShowWindowAsync(h, SW_MINIMIZE);

            LockedWindow lw;
            lw.hwnd  = h;
            lw.pid   = pid;
            lw.title = title;
            g_locked.push_back(lw);
            ++done;

            elog::Write(L"[lockdown] 收起窗口: \"%s\" (pid=%lu)",
                        title.c_str(), (unsigned long)pid);
        }
        return done;
    }

    // 把收走的窗口放回去。句柄可能已经失效、也可能被系统回收给了别的窗口，
    // 所以两样都核对：窗口还在，而且进程号没变。
    int RestoreAll()
    {
        // 记下当前前台窗口——恢复完之后要把焦点还回去。
        // 有些 UWP 应用即使收到"不激活"的显示请求，也会在自己的
        // 激活回调里调 SetForegroundWindow 抢前台，这里最后再压一下。
        HWND prevForeground = GetForegroundWindow();

        int done = 0;
        for (size_t i = 0; i < g_locked.size(); ++i)
        {
            HWND h = g_locked[i].hwnd;
            if (!h || !IsWindow(h)) continue;

            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != g_locked[i].pid) continue;      // 句柄被别人复用了，别动

            // 只还原「确实还收着」的：用户后来自己把它叫出来了，就别再动它，
            // 免得把人家的窗口在散场瞬间又跳一下。
            if (!IsIconic(h)) continue;

            // 用 SW_SHOWNOACTIVATE 而不是 SW_RESTORE。
            //
            // SW_RESTORE 会把窗口恢复**并激活到前台**——恢复被收走的
            // 那批窗口时，最后一个被恢复的会"弹"到屏幕上。如果它是
            // UWP 应用（计算器 / 设置 / 照片 / 应用商店……），视觉上
            // 和「这个程序被打开了」一模一样，用户会以为是本程序干的。
            // 这就是"退出时随机打开一个 UWP 应用"的来源。
            //
            // SW_SHOWNOACTIVATE 只把窗口从最小化恢复成普通状态、
            // **不抢焦点**，观感上只是任务栏那个按钮从按下变回弹起，
            // 屏幕上一个窗口都不会闪出来。
            ShowWindowAsync(h, SW_SHOWNOACTIVATE);
            ++done;
        }

        // 个别 UWP 应用即使收到 SW_SHOWNOACTIVATE 也会自己抢前台
        // （它们的激活逻辑是异步的，不受这个 nCmdShow 约束）。
        // 把焦点还给恢复之前那个窗口，压住它们。
        if (prevForeground && IsWindow(prevForeground) &&
            GetForegroundWindow() != prevForeground)
        {
            SetForegroundWindow(prevForeground);
        }

        return done;
    }

    // 守望：把「我们收走的、但已经被叫回来」的窗口重新收回去。
    int RepressRestored()
    {
        int hit = 0, gaveUp = 0;

        for (size_t i = 0; i < g_locked.size(); ++i)
        {
            LockedWindow& lw = g_locked[i];
            HWND h = lw.hwnd;
            if (!h || !IsWindow(h)) continue;
            if (IsIconic(h)) continue;

            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != lw.pid) continue;

            if (++lw.resist > kGiveUpAfter)
            {
                if (lw.resist == kGiveUpAfter + 1)
                {
                    ++gaveUp;
                    elog::Write(L"[lockdown] 放弃 \"%s\"：它被收起后总自己弹回来（试了 %d 次）",
                                lw.title.c_str(), kGiveUpAfter);
                }
                continue;
            }

            if (hit >= kMaxPerTick) continue;

            // 第一次被弹回来时记一笔是谁 —— 排查「任务栏一直闪」这类问题时，
            // 需要知道到底是哪个窗口在反抗。
            if (lw.resist == 1)
                elog::Write(L"[lockdown] \"%s\" 被叫回来了，重新收起（盯着它）", lw.title.c_str());

            ShowWindowAsync(h, SW_MINIMIZE);
            ++hit;
        }

        if (hit > 0)
        {
            const int total = g_repressed += hit;
            elog::Write(L"[lockdown] 本轮 有 %d 个窗口被叫回来了，已重新收起（累计 %d 次）",
                        hit, total);
        }
        return gaveUp;
    }

} // namespace

namespace lockdown {

void AddOwnClass(const wchar_t* className)
{
    if (!className || !*className) return;
    g_ownClasses.push_back(className);
}

int Start()
{
    g_selfPid = GetCurrentProcessId();

    if (!g_enabled)
    {
        elog::Write(L"[lockdown] 清场已被关闭（--no-lockdown），跳过");
        return 0;
    }

    // 重复进场先把上一轮还原掉，避免 g_locked 里堆两份。
    if (g_active) Restore();

    g_locked.clear();
    g_active   = true;
    g_lastTick = GetTickCount();
    g_repressed = 0;

    const int n = MinimizeEverything();

    elog::Write(L"[lockdown] 清场完成：收走 %d 个窗口（键盘拦截=%d，守望间隔 %lums）",
                n, (int)g_blockKeys, (unsigned long)kRepressMs);
    return n;
}

int Restore()
{
    if (!g_active)
    {
        // 没在清场状态也允许调用：可能是上次被强杀之后的重启路径
        if (g_locked.empty()) return 0;
    }

    g_active = false;
    const int back = RestoreAll();
    const int total = (int)g_locked.size();
    g_locked.clear();

    elog::Write(L"[lockdown] 散场：还原 %d 个（本次共收走 %d 个，期间被叫回来又压回去 %d 次）",
                back, total, g_repressed);
    return back;
}

bool Active()          { return g_active; }
int  LockedCount()     { return (int)g_locked.size(); }
int  RepressedCount()  { return g_repressed; }

void Tick()
{
    if (!g_active) return;

    const DWORD now = GetTickCount();
    if (now - g_lastTick < kRepressMs) return;
    g_lastTick = now;

    const int gaveUp = RepressRestored();
    if (gaveUp > 0)
        elog::Write(L"[lockdown] 本轮放弃 %d 个「收不住」的窗口（剩下的继续看着）", gaveUp);
}

void SetBlockKeys(bool on)
{
    if (g_blockKeys == on) return;
    g_blockKeys = on;
    elog::Write(L"[lockdown] 键盘拦截（Win / Alt+Tab / Ctrl+Esc）：%s", on ? L"开" : L"关");
}

bool BlockKeys() { return g_blockKeys; }

void SetEnabled(bool on)
{
    if (g_enabled == on) return;
    g_enabled = on;
    if (!on && g_active) Restore();     // 中途关掉：立刻放人
    elog::Write(L"[lockdown] 清场功能：%s", on ? L"开" : L"关");
}

bool Enabled() { return g_enabled; }

bool ShouldSwallowKey(DWORD vkCode)
{
    if (!g_active || !g_blockKeys) return false;

    switch (vkCode)
    {
    case VK_LWIN:
    case VK_RWIN:                                  // Win 系全部组合：Win+D/M/Tab/L/E…
    case VK_TAB:                                   // Alt+Tab / Alt+Shift+Tab（配合下面的 ALT 判定）
    case VK_ESCAPE:                                // Ctrl+Esc 开开始菜单
        break;
    default:
        return false;
    }

    // 单独的 Tab / Esc 不能吞 —— 用户可能正在文本框里用它们。
    // 只有和 Alt / Ctrl 一起按下时才算「唤回窗口」的组合键。
    const bool alt   = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
    const bool ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (vkCode == VK_TAB)    return alt;
    if (vkCode == VK_ESCAPE) return ctrl;
    return true;                                   // Win 键本身：直接吞
}

} // namespace lockdown
