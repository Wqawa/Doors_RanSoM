// ============================================================================
//  director.cpp
// ============================================================================
#include "director.h"

#include "desktop_overlay.h"
#include "entity_log.h"
#include "audio.h"
#include "face.h"
#include "fx.h"
#include "gold.h"
#include "lockdown.h"
#include "motion.h"
#include "popup.h"
#include "recycle.h"

#include <cstdlib>
#include <cwchar>

namespace {

    const wchar_t* kClassName = L"RansomDirectorWnd";
    const UINT_PTR kTickId = 1;
    const UINT     kTickMs = 16;

    // 各阶段时长。0 = 停住不自动推进。
    const DWORD kIdleMs = 4000;
    const DWORD kFaceMs = 900;    // 头在任意位置浮现
    // 瞬移到中央 + 停牌出现 + jumpscare2 起播 + 检测启动，都在 CENTER 这一瞬。
    // 停牌显示到 CENTER + STOP 结束（= kStopShowMs），判定也在这时做。
    const DWORD kCenterMs = 200;    // 瞬移到中央；停牌从这一刻开始显示
    const DWORD kStopMs = 800;    // 停牌显示的剩余时长
    const DWORD kEscapedMs = 200;    // 停牌消失后，头再活这么久

    // 停牌总显示时长 = kCenterMs + kStopMs = 1000ms。
    // jumpscare2 从 0.4s 处起播（见 audio.cpp 的 kOneShotStartSec），
    // 正好播满 1 秒时停牌消失——符合「从 0.4 秒开始播放 jumpscare2 后到 1 秒
    // 时牌子消失」的要求。再 kEscapedMs（0.2 秒），头消失。
    const DWORD kStopShowMs = kCenterMs + kStopMs;

    // 被抓之后、勒索窗口弹出之前的两段开场：
    //   前 kJumpscareMs  —— A-90_JUMPSCARE（先小后大）
    //   中 kLoadMs       —— 加载条（进度条必须在这段时间里走完）
    //   末 kFinishMs     —— 加载条满格后，文字变 FINISH！再停这么久
    // kCatchLeadMs 是三段之和，倒计时也是从这一刻才开始（见 RansomElapsedMs）。
    const DWORD kJumpscareMs = 1000;
    const DWORD kLoadMs = 1300;
    const DWORD kFinishMs = 200;
    const DWORD kCatchLeadMs = kJumpscareMs + kLoadMs + kFinishMs;
    const DWORD kCaughtMs = 90000;   // 原作就是 90 秒
    const DWORD kPaidMs = 4800;    // 要盖住 popup 那套付钱演出的全长（约 4.4 秒）
    const DWORD kPunishMs = 2200;

    const DWORD kCooldownPaidMs = 11000;
    const DWORD kCooldownPunishMs = 13000;

    // 倒计时剩这么多的时候进入收尾：叠加播放 Ransom_encounter（riser），
    // **同时**主题曲开始线性渐隐。
    const DWORD kRiserLeadMs = 15000;

    // 玩家每主动关掉一个勒索子窗口，勒索倒计时往前扣这么多。
    // 这是给玩家的一条「别干等着」的出路 —— 也可以主动关窗口拖到超时，
    // 关 9 个就直接触发没付清的跳杀。
    const DWORD kChildCloseCreditMs = 10000;

    // 全屏纯色底的配色
    const COLORREF kCenterVeil = RGB(0, 0, 0);
    const int      kCenterNoise = 80;
    const COLORREF kStopVeil = RGB(170, 0, 0);
    const COLORREF kAttackVeil = RGB(80, 0, 0);

    // 「已加密」阶段的红幕：集中在四个角的红色大噪点像素。
    const COLORREF kLockGlow = RGB(210, 16, 16);
    const int      kLockGlowStrength = 170;

    // 赎金目标（原作 500 Gold）
    const int kGoldGoal = 500;

    HWND         g_hwnd = nullptr;
    director::Phase g_phase = director::PHASE_IDLE;
    DWORD        g_phaseStart = 0;
    DWORD        g_startTick = 0;
    bool         g_auto = true;

    bool         g_caught = false;
    int          g_gold = 0;
    DWORD        g_paidAt = 0;
    DWORD        g_caughtAt = 0;
    bool         g_ransomBegun = false;
    bool         g_loadShown = false;
    bool         g_riserFired = false;
    int          g_fadeLogged = 101;

    // 玩家关子窗口累积的「提前量」。RansomElapsedMs() 会把它加到已过
    // 时间上，所以倒计时、渐隐、riser、超时判定全都自动跟着走。
    // 每一轮被抓（PHASE_CAUGHT 进入）时清零。
    DWORD        g_timeCreditMs = 0;

    DWORD        g_idleMs = kIdleMs;
    DWORD        g_pendingIdleMs = kIdleMs;

    DWORD RansomElapsedMs()
    {
        const DWORD begin = g_caughtAt + kCatchLeadMs;
        const DWORD now = GetTickCount();
        const DWORD el = (now <= begin) ? 0 : (now - begin);

        // 加上玩家关子窗口攒下的提前量。
        // DWORD 溢出不是问题：每次最多 10 秒、子窗口上限 14 个，
        // 满打满算也就 140 秒，离 DWORD 上限差着几个数量级。
        return el + g_timeCreditMs;
    }

    void EnterPhase(director::Phase p);
    void Tick();

    // 判定：**只在停牌显示结束时**调用。
    void Verdict()
    {
        const bool moved = motion::Moved();
        g_caught = moved;

        elog::Write(L"[director] 判定（停牌期间）：%s（鼠标偏移 %dpx，键动=%d，键=0x%02X）",
            moved ? L"动了 → 被抓" : L"没动 → 避开",
            motion::MouseDrift(),
            (int)motion::MovedByKeyboard(),
            motion::LastKey());

        motion::Disarm();
    }

    // ---------------------------------------------------------------------------
    //  阶段进入动作 —— 剧本都在这里
    // ---------------------------------------------------------------------------
    void EnterPhase(director::Phase p)
    {
        g_phase = p;
        g_phaseStart = GetTickCount();

        elog::Write(L"[director] === 阶段 %d 「%s」 ===", (int)p, director::PhaseName(p));

        switch (p)
        {
        case director::PHASE_IDLE:
            g_idleMs = g_pendingIdleMs;
            g_pendingIdleMs = kIdleMs;

            lockdown::Restore();          // 回到潜伏态：确保没有窗口被扣着
            face::Hide();
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            motion::Disarm();
            popup::EndRansom();
            overlay::SetLook(overlay::LOOK_FRAME);
            overlay::SetVisible(false);
            audio::Silence();
            audio::SetMaster(60);
            break;

        case director::PHASE_FACE:
            // 头在屏幕**任意位置**浮现。此刻**不**开始监视、**不**播停牌音。
            g_caught = false;
            g_ransomBegun = false;
            g_gold = 0;
            face::SpawnAnywhere(kFaceMs + kCenterMs + kStopMs);
            audio::SetGlitchBed(6);
            break;

        case director::PHASE_CENTER:
            // 瞬移到屏幕正中央**同时**叠加停牌 + 播停牌音效 + 启动检测。
            // 停牌显示 kStopShowMs = kCenterMs + kStopMs = 1000ms（从瞬移那一刻
            // 算起），正好是 jumpscare2 从 0.4s 处起播后满 1 秒。
            // 头会一直活到 ESCAPED 结束，也就是停牌消失后再显示 kEscapedMs。
            //
            // 注意 headLifeMs 传的是「头显示总时长」，包含 ESCAPED 那段——
            // 但如果判定抓到了，CAUGHT 会立刻切到 ShowAttackStill，覆盖掉
            // 这个 modeEnd；如果没判到，ESCAPED 里的 ShowHeadAgain(kEscapedMs)
            // 也会重设 modeEnd。所以这里的 headLifeMs 只是「保底」值，
            // 保证在判定发生前 face 不会自己 HIDDEN。
            face::MoveToCenterWithStop(kCenterMs + kStopMs + kEscapedMs,
                kStopShowMs);
            motion::Arm();
            audio::PlayGlitch();          // jumpscare2 从 0.4s 处起播

            fx::SetSolid(true, kCenterVeil);   // 黑底
            fx::SetNoise(kCenterNoise);
            break;

        case director::PHASE_STOP:
            // 停牌已经在 CENTER 进入时启动。这里只做底色的变化：
            // 从「中央黑 + 噪点」切到「停牌亮红、无噪点」。
            // 判定发生在本阶段结束时（Tick 里调 Verdict），此时停牌正好
            // 也到了 g_idleStopHideAt——玩家看到停牌消失那一瞬，就知道判没判到。
            fx::SetSolid(true, kStopVeil);
            fx::SetNoise(0);
            break;

        case director::PHASE_ESCAPED:
            // 没动：停牌在 CENTER 进入后 kStopShowMs 就已经自动隐藏了，
            // 这里再撤掉标志、并把头的 modeEnd 重设成「再活 kEscapedMs」。
            // 结果：停牌消失 → 0.2 秒后头消失，符合要求。
            lockdown::Restore();          // 保险：避开这条路不该有窗口被扣着
            face::ShowHeadAgain(kEscapedMs);
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            popup::EndRansom();
            overlay::SetLook(overlay::LOOK_FRAME);
            overlay::SetVisible(false);
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            break;

        case director::PHASE_CAUGHT:
            // 动了：开始两段开场（jumpscare -> 加载条）。
            // face 那边已经在 CENTER 进入时撤掉了停牌（如果停牌已经到点），
            // 这里 ShowAttackStill 会一并把 g_idleShowStop 也归零。
            g_gold = 0;
            g_paidAt = 0;
            g_caughtAt = GetTickCount();
            g_ransomBegun = false;
            g_loadShown = false;
            g_timeCreditMs = 0;            // 每一轮从头攒
            face::ShowAttackStill(kJumpscareMs);
            audio::PlayCaught();
            break;

        case director::PHASE_PAID:
            lockdown::Restore();          // 付清：把收走的窗口放回去
            popup::BeginPayup();
            overlay::SetLook(overlay::LOOK_FRAME);
            overlay::SetVisible(false);
            gold::Cleanup();
            face::Hide();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            g_paidAt = GetTickCount();
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            break;

        case director::PHASE_PUNISH:
            lockdown::Restore();          // 超时受罚：也要放回去，不能把人锁死
            popup::EndRansom();
            gold::Cleanup();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(false);
            face::ShowAttackStill(kPunishMs, false); 
            audio::PlayHit();
            audio::SetGlitchBed(0);
            audio::SetTheme(false);
            recycle::SendToBin();
            break;

        default:
            break;
        }
    }

    void Tick()
    {
        const DWORD now = GetTickCount();
        const DWORD inPhase = now - g_phaseStart;

        // 清场守望：把被叫回来的窗口重新收回去（内部自己限流）。
        // 挂在这里是因为 director 的定时器是全场最稳的心跳（16ms），
        // 覆盖层的定时器在 explorer 重启之类的路上会短暂断掉。
        lockdown::Tick();

        if (g_phase == director::PHASE_CAUGHT && !g_ransomBegun)
        {
            const DWORD el = now - g_caughtAt;

            if (!g_loadShown && el >= kJumpscareMs)
            {
                g_loadShown = true;
                face::ShowLoading(kLoadMs, kFinishMs);
                elog::Write(L"[director] JUMPSCARE 演完，切到加载画面（走条 %lu ms + FINISH %lu ms）",
                    (unsigned long)kLoadMs, (unsigned long)kFinishMs);
            }
        }

        if (g_phase == director::PHASE_CAUGHT && !g_ransomBegun &&
            now - g_caughtAt >= kCatchLeadMs &&
            (!g_loadShown || face::LoadingDone()))
        {
            g_ransomBegun = true;
            g_riserFired = false;
            g_fadeLogged = 101;

            face::Hide();
            fx::SetSolid(false);
            fx::SetNoise(0);
            fx::SetEdgeGlow(true, kLockGlow, kLockGlowStrength);

            popup::BeginRansom(8);
            overlay::SetLook(overlay::LOOK_LOCKED);
            overlay::SetVisible(true);
            gold::Spawn(kGoldGoal);

            // ---- 桌面清场 ----
            // 勒索窗口已经铺开了，这时候把用户原来开着的程序全收进任务栏，
            // 并在勒索期间不让它们回来。放在这一行（而不是被抓的那一瞬）是
            // 有意的：前面的 jumpscare + 加载画面要占满整个屏幕，那两段
            // 本来也看不清桌面，等勒索窗口真的出现时再清场，视觉上更干净。
            lockdown::Start();

            audio::PlayError();
            audio::SetGlitchBed(22);
            audio::SetTheme(true);

            elog::Write(L"[director] 加载结束，进入勒索阶段");
        }

        // 玩家关掉了子窗口？每关一个，倒计时往前扣 10 秒。
        // 放在 SetStatus 之前：这样同一帧里 SetStatus 看到的 remain
        // 就已经减过了，窗口上的时间显示不会慢一拍。
        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
        {
            const int n = popup::ConsumePlayerClosedCount();
            if (n > 0)
            {
                const DWORD credit = (DWORD)n * kChildCloseCreditMs;
                g_timeCreditMs += credit;

                // 音乐也跟着往前跳，维持和倒计时的同步。
                // 不这么做的话主题曲会继续按真实时间慢慢播，
                // 玩家一关窗口就会发现"音乐和倒计时对不上了"。
                audio::SeekThemeBy((double)credit / 1000.0);

                elog::Write(L"[director] 玩家关闭 %d 个子窗口，倒计时提前 %d 秒（累计提前 %.1f 秒）",
                    n, n * (int)(kChildCloseCreditMs / 1000),
                    g_timeCreditMs / 1000.0);
            }
        }
        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
            popup::SetStatus(g_gold, kGoldGoal, director::RansomRemainMs(), kCaughtMs);

        if (g_phase == director::PHASE_CAUGHT && g_gold >= kGoldGoal)
        {
            EnterPhase(director::PHASE_PAID);
            return;
        }

        if (g_phase == director::PHASE_CAUGHT && g_ransomBegun)
        {
            const DWORD remain = director::RansomRemainMs();

            if (remain > 0 && remain < kRiserLeadMs)
            {
                const int level = (int)(100.0 * (double)remain / (double)kRiserLeadMs);
                audio::SetThemeLevel(level);

                if (level <= g_fadeLogged - 10)
                {
                    g_fadeLogged = level;
                    elog::Write(L"[director] 主题曲渐隐至 %d%%（剩 %.1f 秒）",
                        level, remain / 1000.0);
                }

                if (!g_riserFired)
                {
                    g_riserFired = true;
                    audio::PlayRiser();
                    elog::Write(L"[director] 倒计时剩 %.1f 秒，叠加 riser（Ransom_encounter，满音量）",
                        remain / 1000.0);
                }
            }
        }

        if (!g_auto) return;

        if (g_phase == director::PHASE_CAUGHT)
        {
            if (!g_ransomBegun) return;
            if (RansomElapsedMs() < kCaughtMs) return;
            EnterPhase(director::PHASE_PUNISH);
            return;
        }

        DWORD dur = 0;
        switch (g_phase)
        {
        case director::PHASE_IDLE:    dur = g_idleMs;   break;
        case director::PHASE_FACE:    dur = kFaceMs;    break;
        case director::PHASE_CENTER:  dur = kCenterMs;  break;
        case director::PHASE_STOP:    dur = kStopMs;    break;
        case director::PHASE_ESCAPED: dur = kEscapedMs; break;
        case director::PHASE_PAID:    dur = kPaidMs;    break;
        case director::PHASE_PUNISH:  dur = kPunishMs;  break;
        default: break;
        }

        if (dur == 0 || inPhase < dur) return;

        director::Phase next = director::PHASE_IDLE;

        switch (g_phase)
        {
        case director::PHASE_IDLE:    next = director::PHASE_FACE;    break;
        case director::PHASE_FACE:    next = director::PHASE_CENTER;  break;
        case director::PHASE_CENTER:  next = director::PHASE_STOP;    break;
        case director::PHASE_STOP:
            Verdict();                       // 停牌结束 -> 结算
            next = g_caught ? director::PHASE_CAUGHT
                : director::PHASE_ESCAPED;
            break;
        case director::PHASE_ESCAPED: g_pendingIdleMs = kIdleMs;           next = director::PHASE_IDLE; break;
        case director::PHASE_PAID:    g_pendingIdleMs = kCooldownPaidMs;   next = director::PHASE_IDLE; break;
        case director::PHASE_PUNISH:  g_pendingIdleMs = kCooldownPunishMs; next = director::PHASE_IDLE; break;
        default: break;
        }

        EnterPhase(next);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wp == kTickId) { Tick(); return 0; }
            break;
        case WM_ERASEBKGND:
            return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

} // namespace

namespace director {

    bool Start(HINSTANCE hInst)
    {
        if (g_hwnd) return true;

        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = nullptr;
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        g_hwnd = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_hwnd)
        {
            elog::Write(L"[director] 调度窗口创建失败, err=%lu", GetLastError());
            return false;
        }

        g_startTick = GetTickCount();
        SetTimer(g_hwnd, kTickId, kTickMs, nullptr);

        popup::Start(hInst);
        recycle::Start(hInst);

        EnterPhase(PHASE_IDLE);
        return true;
    }

    void Stop()
    {
        if (g_hwnd)
        {
            KillTimer(g_hwnd, kTickId);
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }

        motion::Disarm();
        face::Hide();
        fx::SetNoise(0);
        popup::Stop();
        gold::Cleanup();
        lockdown::Restore();          // 退出前一定要把窗口放回去
        audio::Silence();

        elog::Write(L"[director] 已停止");
    }

    void JumpTo(Phase p)
    {
        if ((int)p < 0 || (int)p >= (int)PHASE_COUNT) return;
        EnterPhase(p);
    }

    Phase Current() { return g_phase; }

    const wchar_t* PhaseName(Phase p)
    {
        switch (p)
        {
        case PHASE_IDLE:    return L"潜伏";
        case PHASE_FACE:    return L"任意位置浮现";
        case PHASE_CENTER:  return L"瞬移到中央";
        case PHASE_STOP:    return L"停牌判定";
        case PHASE_ESCAPED: return L"避开";
        case PHASE_CAUGHT:  return L"被抓";
        case PHASE_PAID:    return L"付清";
        case PHASE_PUNISH:  return L"惩罚";
        default:            return L"?";
        }
    }

    Phase PhaseFromName(const wchar_t* name)
    {
        if (!name || !*name) return PHASE_COUNT;
        if (iswdigit(name[0]))
        {
            const int n = _wtoi(name);
            return (n >= 0 && n < (int)PHASE_COUNT) ? (Phase)n : PHASE_COUNT;
        }
        for (int i = 0; i < (int)PHASE_COUNT; ++i)
            if (wcscmp(name, PhaseName((Phase)i)) == 0) return (Phase)i;
        return PHASE_COUNT;
    }

    void SetAutoAdvance(bool on) { g_auto = on; }
    bool AutoAdvance() { return g_auto; }

    DWORD PhaseElapsedMs() { return GetTickCount() - g_phaseStart; }
    DWORD TotalElapsedMs() { return GetTickCount() - g_startTick; }

    void CreditGold(int amount)
    {
        if (g_phase != PHASE_CAUGHT) return;
        if (amount <= 0) return;

        g_gold += amount;
        elog::Write(L"[director] 收到 %d Gold（累计 %d / %d）", amount, g_gold, kGoldGoal);
    }

    int Gold() { return g_gold; }
    int GoldGoal() { return kGoldGoal; }

    DWORD RansomRemainMs()
    {
        if (g_phase != PHASE_CAUGHT || !g_ransomBegun) return 0;
        const DWORD el = RansomElapsedMs();
        return (el >= kCaughtMs) ? 0 : (kCaughtMs - el);
    }

    bool CaughtThisRound() { return g_caught; }

} // namespace director