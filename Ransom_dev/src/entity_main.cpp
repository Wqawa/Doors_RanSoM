// ============================================================================
//  entity_main.cpp
//
//  Ransom（DOORS）桌面版 —— 进程入口。
//
//  模块：
//    face            实体本体：程序生成的像素化单色扭曲脸 + 停牌
//    motion          移动检测（Ransom 的核心机制：不许动）
//    director        遭遇战调度器
//    desktop_overlay 桌面快捷方式覆盖层（被抓后标记为「已加密」）
//    fx              屏幕特效（红噪 / 闪屏）
//    audio           合成音效
//    entity_log      公共日志
//
//  两条铁律：
//    1. Windows 子系统，wWinMain 是唯一入口。
//    2. 强制退出热键 Ctrl+Alt+Shift+Q **始终有效**，并且被**
//       移动检测永久豁免**——否则玩家按 Ctrl 的那一下就会立刻被抓，
//       安全阀永远按不完。
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

// GDI+ 需要 IStream / PROPID，WIN32_LEAN_AND_MEAN 不会带进来
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#include <cstdlib>
#include <cwchar>
#include <string>

#include "assets.h"
#include "aero_window.h"
#include "ui_layout.h"
#include "desktop_overlay.h"
#include "guardian.h"
#include "lockdown.h"
#include "audio.h"
#include "director.h"
#include "entity_log.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "motion.h"
#include "recycle.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

const wchar_t* kEntityName = L"Ransom_dev";

// 安全阀
const int  kHotkeyPanic = 1;
const UINT kPanicMods   = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
const UINT kPanicVK     = 'Q';

// 用于收热键和跨进程消息的隐藏窗口
const wchar_t* kIpcClass = L"RansomDevIpcWnd";

volatile bool g_running = true;

// 跨进程消息：金币 .lnk 被双击时，由新起的进程发过来
UINT g_msgPay = 0;

// ---------------------------------------------------------------------------
//  付款模式
//
//  被双击的金币 .lnk 会以 `--pay <金额> --token <编号>` 拉起本程序的一份新实例。
//  这个实例**必须立刻发消息然后退出**——绝不能跑下去变成第二个实体。
//  所以它在任何模块启动之前就返回了。
// ---------------------------------------------------------------------------
int SendPayment(int amount, int token)
{
    const UINT msg = RegisterWindowMessageW(L"RansomDev_PayGold");
    if (!msg) return 2;

    HWND h = FindWindowW(L"RansomDevIpcWnd", nullptr);
    if (!h || !IsWindow(h)) return 3;      // 实体没在运行，这次点击作废

    // PostMessage 跨进程只传两个整数，不需要任何内存封送
    PostMessageW(h, msg, (WPARAM)amount, (LPARAM)token);
    return 0;
}

LRESULT CALLBACK IpcProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 守护进程心跳：定期检查守护还在不在，不在就重启一个。
    if (msg == WM_TIMER && wp == 2)
    {
        guardian::Tick();
        return 0;
    }

    if (msg == g_msgPay && g_msgPay != 0)
    {
        const int amount = (int)wp;
        const int token  = (int)lp;

        elog::Write(L"[ipc] 收到付款请求 %d Gold（token=%d）", amount, token);

        // 只有真的找到并删掉了对应的金币文件才计入——防止重复点击刷金额
        const int got = gold::Consume(token);
        if (got > 0)
        {
            director::CreditGold(got);
            audio::PlayCoin();          // 拾取音
        }

        return 0;
    }

    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ------------------------------------------------------------------ 入口 ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // ---- 守护模式：直接进守护循环，不碰主流程 ----
    //
    // 守护进程是同一个 exe 拉起来的自己，只是带了 --guardian <pid> <gen>。
    // 这一段必须在**最前面**：守护进程不该碰高 DPI 设置、命令行解析、
    // 模块启动……它只需要监护主进程，然后在需要时跑一次惩罚演出。
    if (guardian::IsGuardianMode())
    {
        DWORD targetPid = 0;
        int   gen = 0;
        if (!guardian::ParseGuardianArgs(__argc, __wargv, targetPid, gen) ||
            targetPid == 0)
        {
            // 参数坏了（不该发生），直接退，别把守护进程留成孤儿
            return 2;
        }
        return guardian::RunGuardian(hInst, targetPid, gen);
    }

    // ---- 高 DPI ----
    // ---- 高 DPI ----
    {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32)
        {
            PFN_SetProcessDpiAwarenessContext fn =
                (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
            if (!fn || !fn((HANDLE)-4))
            {
                typedef BOOL (WINAPI *PFN_SetProcessDPIAware)(void);
                PFN_SetProcessDPIAware fn2 =
                    (PFN_SetProcessDPIAware)GetProcAddress(u32, "SetProcessDPIAware");
                if (fn2) fn2();
            }
        }
    }

    // ---- 命令行 ----
    //   --phase NAME|N         跳到某阶段并停住
    //   --no-auto              不自动推进
    //   --no-audio             不启动声音
    //   --no-overlay           不启动桌面覆盖层
    //   --overlay-topmost      覆盖层置顶（默认贴桌面层）
    //   --no-block-menu        不拦截被加密图标上的右键菜单（排查问题时用）
    //   --no-lockdown          勒索时不最小化别的程序（排查问题时用）
    //   --no-guardian          不启动双进程看守（调试时用；有调试器时也会自动跳过）
    //   --tolerance N          鼠标容差像素（默认 10）
    //   --face-demo MODE       只显示某张脸：idle/stop/attack/thanks/loading
    //   --face-dump DIR        把程序生成的素材导出成 PNG 后退出（验证外观用）
    //   --clean-gold           只扫描并删除遗留的金币快捷方式，然后退出
    //   --restore              从回收站按清单还原被没收的快捷方式，然后退出
    //   --audio-dump PATH      把全部音效离线渲染成 WAV 后退出（验证波形用）
    //   --theme-dump PATH      把处理后的主题曲导成 WAV 后退出（试听用）
    //   --ui-preview PATH      把勒索主窗口排版渲染成 PNG 后退出（调排版用）
    //   --ui-grid              配合 --ui-preview，预览图叠坐标网格
    //   --fx-demo NAME PATH    设置 fx 图层状态并把它单独导成 PNG 后退出。
    //                          NAME：glow（四角红光）/ black（黑幕+雪花）/ stop（亮红幕）
    //   --image-dir DIR        覆盖图片素材目录（默认自动找 assets\image）
    //   --audio-dir DIR        覆盖音频素材目录（默认自动找 assets\audio）
    //   --diag PATH            写诊断日志
    bool           noAuto      = false;
    bool           noAudio     = false;
    bool           noOverlay   = false;
    bool           overlayTop  = false;
    bool           noBlockMenu = false;
    bool           noLockdown = false;
    bool           noGuardian = false;    
    const wchar_t* startPhase  = nullptr;
    const wchar_t* faceDemo    = nullptr;
    const wchar_t* faceDump    = nullptr;
    const wchar_t* diagPath    = nullptr;
    int            tolerance   = 20;
    int            payAmount   = 0;
    int            payToken    = 0;
    bool           payMode     = false;
    bool           cleanGold   = false;
    bool           doRestore   = false;
    const wchar_t* audioDump   = nullptr;
    const wchar_t* themeDump   = nullptr;
    const wchar_t* uiPreview   = nullptr;
    bool           uiPreviewGrid = false;
    bool           uiPreviewPayup = false;
    const wchar_t* fxDemo      = nullptr;
    const wchar_t* fxDump      = nullptr;
    const wchar_t* audioDir    = nullptr;
    const wchar_t* imageDir    = nullptr;

    for (int i = 1; i < __argc; ++i)
    {
        const wchar_t* a = __wargv[i];
        const bool hasNext = (i + 1 < __argc);

        if (_wcsicmp(a, L"--no-auto") == 0)           noAuto     = true;
        else if (_wcsicmp(a, L"--no-audio") == 0)     noAudio    = true;
        else if (_wcsicmp(a, L"--no-overlay") == 0)   noOverlay  = true;
        else if (_wcsicmp(a, L"--overlay-topmost") == 0) overlayTop = true;
        else if (_wcsicmp(a, L"--no-block-menu") == 0)   noBlockMenu = true;        
        else if (_wcsicmp(a, L"--no-lockdown") == 0)     noLockdown = true;
        else if (_wcsicmp(a, L"--no-guardian") == 0)     noGuardian = true;
        else if (_wcsicmp(a, L"--phase")      == 0 && hasNext) startPhase = __wargv[++i];
        else if (_wcsicmp(a, L"--face-demo")  == 0 && hasNext) faceDemo   = __wargv[++i];
        else if (_wcsicmp(a, L"--face-dump")  == 0 && hasNext) faceDump   = __wargv[++i];
        else if (_wcsicmp(a, L"--diag")       == 0 && hasNext) diagPath   = __wargv[++i];
        else if (_wcsicmp(a, L"--tolerance")  == 0 && hasNext) tolerance  = _wtoi(__wargv[++i]);
        else if (_wcsicmp(a, L"--pay")        == 0 && hasNext) { payAmount = _wtoi(__wargv[++i]); payMode = true; }
        else if (_wcsicmp(a, L"--token")      == 0 && hasNext) payToken   = _wtoi(__wargv[++i]);
        else if (_wcsicmp(a, L"--clean-gold") == 0)            cleanGold  = true;
        else if (_wcsicmp(a, L"--restore")    == 0)            doRestore  = true;
        else if (_wcsicmp(a, L"--audio-dump") == 0 && hasNext) audioDump  = __wargv[++i];
        else if (_wcsicmp(a, L"--theme-dump") == 0 && hasNext) themeDump  = __wargv[++i];
        else if (_wcsicmp(a, L"--ui-preview") == 0 && hasNext) uiPreview = __wargv[++i];
        else if (_wcsicmp(a, L"--ui-grid") == 0)               uiPreviewGrid = true;
        else if (_wcsicmp(a, L"--payup") == 0)                 uiPreviewPayup = true;
        else if (_wcsicmp(a, L"--fx-demo")    == 0 && hasNext) fxDemo = __wargv[++i];
        else if (_wcsicmp(a, L"--fx-dump")    == 0 && hasNext) fxDump = __wargv[++i];
        else if (_wcsicmp(a, L"--audio-dir")  == 0 && hasNext) { audioDir = __wargv[++i]; assets::UseDiskDir(assets::KIND_AUDIO, audioDir); }
        else if (_wcsicmp(a, L"--image-dir")  == 0 && hasNext) { imageDir = __wargv[++i]; assets::UseDiskDir(assets::KIND_IMAGE, imageDir); }
    }

    // ---- 付款模式：发完消息立刻退出，不启动任何模块 ----
    if (payMode)
    {
        const int rc = SendPayment(payAmount, payToken);
        return rc;
    }

    elog::Open(diagPath);
    elog::Write(L"===== Ransom_dev 启动 =====");

    // ---- 手动清理模式：扫掉所有遗留的金币快捷方式然后退出 ----
    if (cleanGold)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        gold::Start(hInst);
        const int n = gold::CleanupStale();
        gold::Stop();
        if (SUCCEEDED(hr)) CoUninitialize();
        elog::Write(L"清理模式：删除 %d 个遗留金币", n);
        elog::Close();
        return 0;
    }

    // ---- 音效导出模式：离线渲染成 WAV 然后退出 ----
    if (audioDump)
    {
        if (!audio::Start()) { elog::Write(L"音频设备不可用，无法导出"); elog::Close(); return 1; }
        const bool ok = audio::DumpMix(audioDump, 10);
        audio::Stop();
        elog::Write(L"音效导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    // ---- 主题曲导出：把处理后的主题曲导成 WAV 后退出（试听用）----
    // 不需要音频设备，Start 失败（没有声卡）也照样能导。
    if (themeDump)
    {
        audio::Start();
        const bool ok = audio::DumpTheme(themeDump);
        audio::Stop();
        elog::Write(L"主题曲导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    // ---- 还原模式：按清单把被没收的快捷方式从回收站放回原位 ----
    if (doRestore)
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        recycle::Start(hInst);
        const int n = recycle::Restore();
        if (SUCCEEDED(hr)) CoUninitialize();
        elog::Write(L"还原模式：放回 %d 个", n);
        elog::Close();
        return 0;
    }

    // ---- GDI+ ----
    // 进程级只需初始化一次。face / fx / overlay 都靠它，
    // 漏了这一步所有 GDI+ 调用都会卡死（不是报错，是挂住）。
    GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    if (GdiplusStartup(&gdipToken, &gsi, nullptr) != Gdiplus::Ok)
    {
        elog::Write(L"GDI+ 初始化失败");
        elog::Close();
        return 1;
    }

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- 主窗口排版预览：渲染成 PNG 后退出（调排版用，不必跑整场演出）----
    // 加 --ui-grid 会叠一层 20px 网格 + 每 100px 的坐标标注，方便读坐标。
    // 放在这里是因为它需要 GDI+ 已经初始化，但不需要任何窗口/模块。
    if (uiPreview)
    {
        ui_layout::Load(L"main_window.ini", L"payup.ini");
        if (uiPreviewPayup) ui_layout::SetActive(1);

        // 造一组示例数值，让 {left} / {time} 这些占位符有东西可显示
        ui_layout::Status st;
        st.gold     = 375;
        st.goal     = 500;
        st.remainMs = 83'000;

        const bool ok = ui_layout::Preview(uiPreview, st, uiPreviewGrid);
        ui_layout::Shutdown();
        elog::Write(L"排版预览结束，成功=%d", (int)ok);
        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }

    // ---- 隐藏的 IPC / 热键窗口 ----
    WNDCLASSEXW ic = { sizeof(WNDCLASSEXW) };
    ic.lpfnWndProc   = IpcProc;
    ic.hInstance     = hInst;
    ic.hCursor       = nullptr;
    ic.hbrBackground = nullptr;
    ic.lpszClassName = kIpcClass;
    RegisterClassExW(&ic);

    HWND hIpc = CreateWindowExW(0, kIpcClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hIpc)
    {
        elog::Write(L"IPC 窗口创建失败, err=%lu", GetLastError());
        if (SUCCEEDED(hrCom)) CoUninitialize();
        elog::Close();
        return 1;
    }

    g_msgPay = RegisterWindowMessageW(L"RansomDev_PayGold");

    // ---- 安全阀热键 ----
    if (!RegisterHotKey(hIpc, kHotkeyPanic, kPanicMods, kPanicVK))
    {
        elog::Write(L"!! 安全阀热键注册失败, err=%lu", GetLastError());
        MessageBoxW(nullptr,
                    L"强制退出热键 Ctrl+Alt+Shift+Q 注册失败（可能被其它程序占用）。\n\n"
                    L"请关掉占用者后重开。",
                    kEntityName, MB_ICONWARNING | MB_OK);
    }
    else
    {
        elog::Write(L"安全阀热键已注册: Ctrl+Alt+Shift+Q（已从移动判定中豁免）");
    }

    // ---- UI 外观资源：图标 + 字体 ----
    // 两样都**内嵌在 exe 里**（图标是 Ransom_dev.rc 的图标资源，
    // 字体是 assets_gen.rc 里的 RODATA）。
    // 字体注册失败就保持默认的 Microsoft YaHei，不会崩。
    // 必须在第一个 aero 窗口创建**之前**设好——窗口类只注册一次，
    // 类的 hIcon 就是在那一刻定下来的。
    aero::SetAppIconFromSelf();
    {
        assets::Blob fnt;
        if (assets::Get(assets::KIND_ROOT, L"RobotoMono-VariableFont_wght.ttf", fnt))
            aero::SetTitleFontFromMemory(fnt.Data(), fnt.Size(), L"Roboto Mono");
        else
            elog::Write(L"字体素材不在包里，标题栏回退 Microsoft YaHei");
    }

    // ---- 各模块启动 ----
    face::Start(hInst);

    // ---- 只导出素材然后退出（开发时用肉眼检查脸长什么样）----
    if (faceDump)
    {
        const bool ok = face::DumpAssets(faceDump);
        face::Stop();
        UnregisterHotKey(hIpc, kHotkeyPanic);
        DestroyWindow(hIpc);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        GdiplusShutdown(gdipToken);
        elog::Write(L"素材导出结束，成功=%d", (int)ok);
        elog::Close();
        return ok ? 0 : 1;
    }

    motion::SetTolerance(tolerance);
    motion::Start(hInst, kPanicVK, true, true, true);
    fx::Start(hInst);
    gold::Start(hInst);
    gold::CleanupStale();          // 清掉上次被强杀留下的残渣
    if (!noAudio) audio::Start();   // 素材已内嵌；解码失败不致命

    const bool overlayOn = !noOverlay && overlay::Start(hInst, overlayTop);
    if (overlayOn)
    {
        overlay::SetVisible(false);      // 只有被抓时才亮出来
        // 被加密的图标上不给右键菜单（默认开，见 desktop_overlay.h）
        if (noBlockMenu) overlay::SetBlockContextMenu(false);
    }
    // 勒索时的桌面清场（默认开，见 lockdown.h）
    if (noLockdown) lockdown::SetEnabled(false);

    // ---- 双进程看守 ----
    // 每次正常启动都清一下「上一轮惩罚已经跑过」的标记，免得上一轮
    // 遗留的标记把这一轮的惩罚吞掉。
    guardian::ClearPunishMark();

    // 调试器下**不启动**守护进程。
    //
    // 理由：VS 的"停止调试"（Shift+F5）、"停止"工具栏按钮，走的是
    // TerminateProcess，**绕过一切清理代码** —— 包括 Disarm。守护这边
    // 看到的就变成"进程消失但 Disarm 没 signal"，等同于被强杀，
    // 于是立即跑惩罚演出（jumpscare + 桌面快捷方式进回收站）。
    // 用调试器开发时几乎每次退出都会误触发。
    //
    // IsDebuggerPresent() 只对"进程正在被调试"返回真 —— 普通用户用
    // 任务管理器杀进程走不到这条分支（他们没挂调试器），所以这条
    // 跳过逻辑**不会削弱对真实强杀的防护**。
    if (noGuardian)
    {
        elog::Write(L"[main] --no-guardian：跳过守护进程启动");
    }
    else if (IsDebuggerPresent())
    {
        elog::Write(L"[main] 检测到调试器，跳过守护进程启动"
            L"（避免 Shift+F5 等调试退出被误判成强杀）");
    }
    else
    {
        guardian::Start(hInst);

        // 主消息循环里定期检查守护进程还在不在，用 Ipc 窗口的定时器挂上。
        // id 用 2（1 是给将来的用途预留的），5000ms 一次足够 —— guardian::Tick
        // 里自己限流到 1000ms，这里只是提供一个心跳源。
        SetTimer(hIpc, 2, 5000, nullptr);
    }

    // 主消息循环里定期检查守护进程还在不在，用 Ipc 窗口的定时器挂上。
    // id 用 2（1 是给将来的用途预留的），5000ms 一次足够 —— guardian::Tick
    // 里自己限流到 1000ms，这里只是提供一个心跳源。
    SetTimer(hIpc, 2, 5000, nullptr);

    // ---- fx 图层导出（开发时看效果用）----
    // fx 是全屏置顶的分层窗口，截屏会被底下的桌面内容污染，
    // 只有把这一层单独导成 PNG 才能按像素数清红光范围和彩色噪点。
    if (fxDump)
    {
        if (fxDemo && _wcsicmp(fxDemo, L"glow") == 0)
        {
            fx::SetEdgeGlow(true, RGB(210, 16, 16), 170);
        }
        else if (fxDemo && _wcsicmp(fxDemo, L"black") == 0)
        {
            fx::SetSolid(true, RGB(0, 0, 0));
            fx::SetNoise(80);
        }
        else if (fxDemo && _wcsicmp(fxDemo, L"stop") == 0)
        {
            fx::SetSolid(true, RGB(170, 0, 0));
        }

        // 第一次 Render 是窗口创建后立刻做的，等一小会儿让图层就位
        Sleep(120);
        const bool ok = fx::DumpLayer(fxDump);
        elog::Write(L"fx 图层导出结束（%s），成功=%d", fxDemo ? fxDemo : L"?", (int)ok);

        fx::ClearAll();
        fx::Stop();
        face::Stop();
        motion::Stop();
        gold::Stop();
        ui_layout::Shutdown();
        audio::Stop();
        if (overlayOn) overlay::Stop();

        // 这条路径走的是"提前返回"，**不经过主消息循环末尾那段收尾代码**，
        // 所以必须在这里自己 Disarm —— 否则守护进程会以为主进程是被强杀的，
        // 立刻跑惩罚演出。
        guardian::Disarm();
        guardian::Stop();

        elog::Close();
        GdiplusShutdown(gdipToken);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return ok ? 0 : 1;
    }

    // ---- 只显示某张脸（开发时看效果用）----
    if (faceDemo)
    {
        elog::Write(L"面部演示模式: %s", faceDemo);
        if      (_wcsicmp(faceDemo, L"idle")   == 0) face::SpawnIdle(0);
        else if (_wcsicmp(faceDemo, L"stop")   == 0) face::ShowStopSign(0);
        else if (_wcsicmp(faceDemo, L"attack") == 0) face::ShowAttack(0);
        else if (_wcsicmp(faceDemo, L"thanks") == 0) face::ShowThanks(0);
        // 加载画面演示：故意把走条拉到 4 秒，方便截图看进度曲线、
        // 抖动幅度和条宽（正常演出里这一段只有 1.3 秒，很难抓帧）。
        else if (_wcsicmp(faceDemo, L"loading") == 0) face::ShowLoading(4000, 800);
    }
    else
    {
        // ---- 遭遇战调度器 ----
        if (director::Start(hInst))
        {
            if (noAuto) director::SetAutoAdvance(false);

            if (startPhase)
            {
                const director::Phase p = director::PhaseFromName(startPhase);
                if (p == director::PHASE_COUNT) elog::Write(L"未知阶段名: %s", startPhase);
                else                            director::JumpTo(p);
            }
        }
    }

    elog::Write(L"进入消息循环");

    // ---- 消息循环 ----
    MSG msg;
    while (g_running && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_HOTKEY)
        {
            if (msg.wParam == kHotkeyPanic)
            {
                elog::Write(L"安全阀触发，退出");
                g_running = false;
                break;
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- 收尾 ----
    elog::Write(L"===== 开始收尾 =====");

    // 双进程看守：先 Disarm（告诉守护这次是正常退出），再关句柄。
    //
    // 顺序很重要：先 SetEvent 再 CloseHandle，最后进程才真正退出。
    // 这样守护那边看到的是「Disarm signaled」，而不是「进程消失但
    // Disarm 没 signal」的强杀状态。
    //
    // 放在收尾最前面：后面那些模块收尾可能需要几秒，早一点告诉守护
    // 它就可以早一点退出，不用一直悬着。
    guardian::Disarm();
    guardian::Stop();

    if (faceDemo) {}
    else { director::Stop(); }
    motion::Stop();
    face::Stop();
    fx::Stop();
    gold::Stop();                  // 删掉全部生成的金币文件
    ui_layout::Shutdown();         // 释放排版用的图片缓存（要在 GdiplusShutdown 之前）
    audio::Stop();
    if (overlayOn) overlay::Stop();

    UnregisterHotKey(hIpc, kHotkeyPanic);
    DestroyWindow(hIpc);

    if (SUCCEEDED(hrCom)) CoUninitialize();
    GdiplusShutdown(gdipToken);

    elog::Write(L"===== 已退出 =====");
    elog::Close();
    return 0;
}
