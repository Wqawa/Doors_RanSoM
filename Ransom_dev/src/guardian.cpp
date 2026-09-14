// ============================================================================
//  guardian.cpp
// ============================================================================
#include "guardian.h"

#include "audio.h"
#include "entity_log.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "recycle.h"

#include <cstdlib>
#include <cwchar>
#include <string>

#include <objbase.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

namespace {

    // 命名事件的公共前缀。用 Local\ 前缀避免跨会话撞名
    // （多用户环境里各人有各人的会话，不该互相干扰）。
    const wchar_t* kDisarmPrefix = L"Local\\RansomDev_Disarm_";
    const wchar_t* kPunishMarkName = L"ransom_dev_punish.done";

    // 主进程侧的状态
    HANDLE g_disarmEvent = nullptr;   // 主进程自己的 Disarm 事件
    HANDLE g_guardianProcess = nullptr;   // 守护进程句柄
    DWORD  g_guardianPid = 0;
    DWORD  g_lastCheckTick = 0;
    bool   g_disarmed = false;

    // ---------------------------------------------------------------- 工具 ----

    void MakeDisarmName(DWORD pid, wchar_t* buf, size_t cch)
    {
        swprintf_s(buf, cch, L"%s%lu", kDisarmPrefix, (unsigned long)pid);
    }

    // 打开一个命名事件（只要 SYNCHRONIZE 就够了 —— 我们只等它 signaled）。
    HANDLE OpenDisarmEvent(DWORD pid)
    {
        wchar_t name[128];
        MakeDisarmName(pid, name, _countof(name));
        return OpenEventW(SYNCHRONIZE, FALSE, name);
    }

    // 创建自己的 Disarm 事件。
    // 名字里带 pid，pid 复用的情况下会打开到同一个事件 —— 用 CREATE_ALWAYS
    // 语义（自动重置）比 CreateEventW 更好，但 Win32 没这个标志，
    // 所以用 SetEvent 之前先 ResetEvent 抹掉旧状态。
    HANDLE CreateDisarmEvent(DWORD pid)
    {
        wchar_t name[128];
        MakeDisarmName(pid, name, _countof(name));
        HANDLE h = CreateEventW(nullptr, TRUE, FALSE, name);
        if (h) ResetEvent(h);
        return h;
    }

    // 「惩罚已经跑过」的标记文件路径。失败返回空串。
    std::wstring PunishMarkPath()
    {
        wchar_t dir[MAX_PATH] = {};
        if (!GetTempPathW(_countof(dir), dir)) return L"";
        std::wstring p = dir;
        p += kPunishMarkName;
        return p;
    }

    bool PunishAlreadyRan()
    {
        const std::wstring p = PunishMarkPath();
        if (p.empty()) return false;
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void MarkPunishRan()
    {
        const std::wstring p = PunishMarkPath();
        if (p.empty()) return;
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    std::wstring ExePath()
    {
        wchar_t buf[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        return buf;
    }

    // 拉起一个守护进程。generation 见 guardian.h。
    // 成功返回进程句柄（调用方负责 CloseHandle），失败返回 nullptr。
    HANDLE SpawnGuardian(DWORD targetPid, int generation)
    {
        const std::wstring exe = ExePath();
        if (exe.empty()) return nullptr;

        // CreateProcessW 的 lpCommandLine 要求可写缓冲，所以拷一份
        wchar_t cmd[512];
        swprintf_s(cmd, L"\"%s\" --guardian %lu %d",
            exe.c_str(), (unsigned long)targetPid, generation);

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi = {};

        // 这是 GUI 子系统程序，不会弹控制台，所以不用 CREATE_NO_WINDOW。
        // 不加 CREATE_BREAKAWAY_FROM_JOB：万一被别的 job 关着，
        // 加这个标志反而会失败、拉不起来。
        if (!CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &si, &pi))
        {
            elog::Write(L"[guardian] CreateProcess 失败（目标 %lu gen %d），err=%lu",
                (unsigned long)targetPid, generation, GetLastError());
            return nullptr;
        }

        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    // 进程还活着吗？用 SYNCHRONIZE 句柄 + 0 超时 Wait。
    // 拿不到句柄（进程不存在 / 权限不足）一律当「不在了」。
    bool ProcessAlive(DWORD pid)
    {
        HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!h) return false;
        const DWORD r = WaitForSingleObject(h, 0);
        CloseHandle(h);
        return (r == WAIT_TIMEOUT);
    }

} // namespace

// ============================================================================
namespace guardian {

    void ClearPunishMark()
    {
        const std::wstring p = PunishMarkPath();
        if (!p.empty()) DeleteFileW(p.c_str());
    }

    // ---------------------------------------------------------------- 主进程侧 ----

    bool Start(HINSTANCE /*hInst*/)
    {
        if (g_disarmEvent) return true;   // 已经起来过了

        const DWORD myPid = GetCurrentProcessId();

        // 自己的 Disarm 事件（监视我的人靠它判断我是不是正常退出）
        g_disarmEvent = CreateDisarmEvent(myPid);
        if (!g_disarmEvent)
        {
            elog::Write(L"[guardian] 创建 Disarm 事件失败, err=%lu", GetLastError());
            // 事件建不出来就别启动守护了：守护会误把每次正常退出都当成强杀，
            // 于是每次关程序都跑一遍惩罚。
            return false;
        }

        g_guardianProcess = SpawnGuardian(myPid, 0);
        if (!g_guardianProcess)
        {
            elog::Write(L"[guardian] 守护进程启动失败 —— 本进程被强杀时不会触发惩罚");
            return false;
        }

        g_guardianPid = GetProcessId(g_guardianProcess);
        g_lastCheckTick = GetTickCount();

        elog::Write(L"[guardian] 守护进程已启动 pid=%lu（本进程 %lu）",
            (unsigned long)g_guardianPid, (unsigned long)myPid);
        return true;
    }

    void Disarm()
    {
        if (g_disarmed) return;
        g_disarmed = true;

        if (g_disarmEvent) SetEvent(g_disarmEvent);
        elog::Write(L"[guardian] 已 Disarm —— 守护不会因为本进程退出而触发惩罚");
    }

    void Tick()
    {
        if (!g_disarmEvent) return;      // Start 失败过，不折腾

        const DWORD now = GetTickCount();
        if (now - g_lastCheckTick < 1000) return;   // 一秒看一次就够
        g_lastCheckTick = now;

        if (g_guardianProcess)
        {
            const DWORD r = WaitForSingleObject(g_guardianProcess, 0);
            if (r == WAIT_TIMEOUT) return;   // 还活着

            // 守护死了：关掉旧句柄，重新拉一个
            CloseHandle(g_guardianProcess);
            g_guardianProcess = nullptr;
            g_guardianPid = 0;
            elog::Write(L"[guardian] 守护进程已消失，重新拉起一个");
        }

        const DWORD myPid = GetCurrentProcessId();
        g_guardianProcess = SpawnGuardian(myPid, 0);
        if (g_guardianProcess)
        {
            g_guardianPid = GetProcessId(g_guardianProcess);
            elog::Write(L"[guardian] 守护进程已重建 pid=%lu", (unsigned long)g_guardianPid);
        }
    }

    void Stop()
    {
        if (g_guardianProcess)
        {
            CloseHandle(g_guardianProcess);
            g_guardianProcess = nullptr;
        }
        if (g_disarmEvent)
        {
            CloseHandle(g_disarmEvent);
            g_disarmEvent = nullptr;
        }
        g_guardianPid = 0;
    }

    // ---------------------------------------------------------------- 守护进程侧 ----

    bool IsGuardianMode()
    {
        for (int i = 1; i < __argc; ++i)
            if (_wcsicmp(__wargv[i], L"--guardian") == 0) return true;
        return false;
    }

    bool ParseGuardianArgs(int argc, wchar_t** argv, DWORD& targetPid, int& generation)
    {
        targetPid = 0;
        generation = 0;

        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"--guardian") != 0) continue;
            if (i + 1 >= argc) return false;

            targetPid = (DWORD)_wtoi(argv[i + 1]);
            if (targetPid == 0) return false;

            // 下一个参数如果不是 "--" 开头，就当 generation。
            // 不这么判的话，"--guardian 1234 --diag log.txt" 会把 "--diag" 当 gen。
            if (i + 2 < argc && argv[i + 2][0] != L'-')
                generation = _wtoi(argv[i + 2]);

            return true;
        }
        return false;
    }

    // 守护进程里跑一遍「没付清」的惩罚演出。
    //
    // 这里**不**复用 director —— 那是主进程的状态机，它依赖的 popup、
    // gold、lockdown 等模块在守护进程里都没起来，状态对不上。所以走一个
    // 独立、自包含的简化版：
    //   1. 全屏红黑底 + 雪花（和主进程 PHASE_PUNISH 的配色一致）
    //   2. 张口脸 jumpscare + Glitchyhitfaster 音效
    //   3. 桌面快捷方式进回收站
    //   4. 给演出留够时间，收尾退出
    //
    // 用户看到的效果和主进程里走 PHASE_PUNISH 几乎一样。
    static void RunPunishShow()
    {
        elog::Write(L"[guardian] === 开始惩罚演出 ===");

        Gdiplus::GdiplusStartupInput gsi;
        ULONG_PTR gdipToken = 0;
        const bool gdipOk =
            (Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr) == Gdiplus::Ok);

        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        HINSTANCE hInst = GetModuleHandleW(nullptr);

        // 这些模块在守护进程里都是「第一次启动」，互不依赖主进程的状态。
        if (gdipOk)
        {
            face::Start(hInst);
            fx::Start(hInst);
        }
        gold::Start(hInst);        // 让 recycle 能靠 IsOurCoinFile 过滤掉自己的金币
        recycle::Start(hInst);
        audio::Start();

        // 全屏红黑底 + 雪花
        fx::SetSolid(true, RGB(80, 0, 0));
        fx::SetNoise(55);

        face::ShowAttack(4000);
        audio::PlayHit();

        // 桌面快捷方式进回收站
        const int n = recycle::SendToBin();
        elog::Write(L"[guardian] 回收站：收走 %d 个快捷方式", n);

        // 跑消息循环等演出走完。
        // face 的 jumpscare 定时器、fx 的雪花定时器都挂在这个循环上。
        const DWORD start = GetTickCount();
        MSG msg;
        while (GetTickCount() - start < 4500)
        {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }

        // 收尾
        fx::ClearAll();
        face::Hide();
        audio::Silence();

        if (gdipOk) fx::Stop();
        if (gdipOk) face::Stop();
        audio::Stop();
        recycle::Stop();
        gold::Stop();

        if (SUCCEEDED(hrCom)) CoUninitialize();
        if (gdipOk) Gdiplus::GdiplusShutdown(gdipToken);

        elog::Write(L"[guardian] === 惩罚演出结束 ===");
    }

    int RunGuardian(HINSTANCE hInst, DWORD targetPid, int generation)
    {
        // 守护进程写自己的日志（带 pid），免得和主进程那份混在一起。
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(_countof(tempDir), tempDir);

        wchar_t logPath[MAX_PATH];
        swprintf_s(logPath, L"%sransom_dev_guardian_%lu.log",
            tempDir, (unsigned long)GetCurrentProcessId());
        elog::Open(logPath);

        elog::Write(L"[guardian] 守护进程启动：pid=%lu 目标=%lu 代次=%d",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long)targetPid, generation);

        // 自己的 Disarm 事件（监视我的人靠它判断我是不是正常退出）
        HANDLE myDisarm = CreateDisarmEvent(GetCurrentProcessId());
        if (!myDisarm)
            elog::Write(L"[guardian] 自己的 Disarm 事件创建失败, err=%lu", GetLastError());

        // 目标的 Disarm 事件（拿不到不是错：目标可能还没来得及创建）
        HANDLE targetDisarm = OpenDisarmEvent(targetPid);

        int exitCode = 0;

        // 目标已经没了？（罕见，但如果主进程刚启动就被杀就可能撞上）
        if (!ProcessAlive(targetPid))
        {
            elog::Write(L"[guardian] 目标进程 %lu 一启动就没了",
                (unsigned long)targetPid);
            if (!PunishAlreadyRan())
            {
                MarkPunishRan();
                RunPunishShow();
            }
            if (myDisarm) { SetEvent(myDisarm); CloseHandle(myDisarm); }
            if (targetDisarm) CloseHandle(targetDisarm);
            elog::Close();
            return exitCode;
        }

        // ---- 等目标退出 ----
        // 轮询而不是用 WaitForMultipleObjects：目标可能 Disarm 之后还要花
        // 一会儿才真正退，需要区分「Disarm 了但还活着」和「进程真没了」，
        // 轮询写起来更直白。
        bool targetDisarmed = false;
        for (;;)
        {
            // Disarm 事件 signaled 了？记下来（目标可能在走正常退出流程）
            if (targetDisarm &&
                WaitForSingleObject(targetDisarm, 0) == WAIT_OBJECT_0)
            {
                targetDisarmed = true;
            }

            if (!ProcessAlive(targetPid))
                break;

            Sleep(500);
        }

        elog::Write(L"[guardian] 目标已退出：disarmed=%d", (int)targetDisarmed);

        if (targetDisarmed)
        {
            elog::Write(L"[guardian] 目标是正常退出，守护一并退出（不触发惩罚）");
        }
        else if (PunishAlreadyRan())
        {
            elog::Write(L"[guardian] 惩罚已经跑过了，跳过");
        }
        else
        {
            // 先把「惩罚已经跑过」写下来，再开始跑。
            // 这样即使我在跑的过程中被强杀，保活进程也不会把惩罚再跑一遍。
            MarkPunishRan();

            // 只有「主守护」（generation 0）才创建保活进程。
            // 保活守护自己（generation 1）不再递归创建 —— 否则无限套娃。
            if (generation == 0)
            {
                HANDLE keepAlive = SpawnGuardian(GetCurrentProcessId(), 1);
                if (keepAlive)
                {
                    elog::Write(L"[guardian] 保活进程已启动 pid=%lu",
                        (unsigned long)GetProcessId(keepAlive));
                    CloseHandle(keepAlive);
                }
                else
                {
                    elog::Write(L"[guardian] 保活进程启动失败 —— 惩罚期间被强杀就没有接力了");
                }
            }

            RunPunishShow();
        }

        // 正常退出：SetEvent 通知监视我的人（保活进程）
        if (myDisarm) SetEvent(myDisarm);

        if (targetDisarm) CloseHandle(targetDisarm);
        if (myDisarm) CloseHandle(myDisarm);
        elog::Close();

        return exitCode;
    }

} // namespace guardian