// ============================================================================
//  popup.cpp
// ============================================================================
#include "popup.h"

#include "aero_window.h"
#include "assets.h"
#include "audio.h"
#include "entity_log.h"
#include "face.h"
#include "ui_layout.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

    const wchar_t* kDriverClass = L"RansomPopupDriver";
    const UINT_PTR kTickId = 1;
    const UINT     kTickMs = 100;

    // ---- A90 的弹窗节奏 ----
    const DWORD kBurstLifeMs = 12000;   // 开局那批子窗口活多久
    const DWORD kGapMinMs = 5000;    // 之后每隔多久冒一批
    const DWORD kGapMaxMs = 12000;
    const DWORD kChildLifeMin = 4000;    // 每个子窗口活多久
    const DWORD kChildLifeMax = 8000;
    const int   kMaxChildren = 14;

    // ---- 主窗口每隔一段时间换个位置 ----
    const DWORD kMainMoveMinMs = 6000;
    const DWORD kMainMoveMaxMs = 13000;
    const DWORD kMainMoveDurMs = 900;    // 单次移动的动画时长
    // 主窗口换位置时，目标点允许的范围相对工作区再内缩多少 px。
    // 取 0 = 让窗口能走遍整个工作区（左上角贴到屏幕边、右下角贴到底），
    // 这已经是「不出屏幕」前提下的最大范围了。
    //
    // 之所以以前要留 60：弧线在中途会把窗口甩到位移两侧，留出行程才不会出界。
    // 现在改成由 aero 在动画期间**每帧把位置夹回工作区**（见 StartMove），
    // 所以这里不需要再留余量——贴边时弧线会被压平，但绝不越界。
    const int   kMainMoveMargin = 0;

    // 等宽文本用的字体族。素材里的 RobotoMono 由 entity_main 经 aero::SetTitleFont
    // 注册；拿不到 FontFamily 就退回 Consolas。
    // 注意**不要**拿它去画中文：Roboto Mono 没有汉字字形，会出豆腐块。
    const wchar_t* kUiMonoFont = L"Roboto Mono";

    // ---- 付完钱的演出 ----
    // 阶段（时间驱动，都在 Tick 里推进）：
    //   TRAVEL   主窗口只留 A90 的头 + 黑底，弧线飞向屏幕中心并缩放到付钱窗口大小
    //   SHOW     切换到付钱排版（背景变 payup_bg），出场动画从 0 开始；
    //            Accepta90 先冒出来（同时播 ransom_success），Thankyou_sign 紧随其后
    //   CLOSING  两个都出场完并停留片刻后，播**原本的关闭动画**收场
    const DWORD kPayTravelMs = 1000;   // 飞过去 + 变大小的时长
    const DWORD kPayCloseAtMs = 3200;   // 切到付钱排版之后多久开始关闭

    // 飞过去的过程**故意不做成平滑插值**，而是切成 10 帧一格一格地跳。
    // 理由：这是一台老机器上的街机式演出，10 帧的阶梯感 + 过冲（冲过中心再弹回来）
    // 比 60fps 的平滑位移更「硬」，和整套 CR T 味的噪点/抖动对得上；
    // 而且总时长仍然是 1000ms，节奏没变，只是走法变了。
    const int kPayTravelFrames = 10;

    enum PayStage { PAY_NONE = 0, PAY_TRAVEL, PAY_SHOW, PAY_CLOSING };
    PayStage g_payStage = PAY_NONE;
    DWORD    g_payStageAt = 0;

    // 倒计时快归零时的间隔与批量。开场先给玩家喘口气，收尾再收紧，
    // 这样 90 秒才是一条曲线而不是一段平铺。
    const DWORD kGapMinEndMs = 2500;
    const DWORD kGapMaxEndMs = 6000;

    // 子窗口标题：照搬原作 Ransom 的弹窗标题
    const wchar_t* kTitles[] = {
        L"Untitled", L"Untitled (2)", L"Untitled (3)",
        L"RANSOM.exe", L"RANASOM", L"MOSNAR",
        L"I FOUND YOU", L"", L"RANSOMRANSOM", L"RRAANNSSOOMM",
        L"LACKLUSTER", L"INCOMPETENT", L"YOU ARE AN IDIOT",
        L"NONIMPRESSIVE", L"ENCRYPTED",
        L"AdWBXV Rk1PRVJNR09PUlRJVEVFU04=",
        L"times up", L"_____", L"YOUR GOLD IS VERY YUMMY!",
        L"YOURGOLDAREBELONGTOUS", L"ERROR", L"Error. Not found.",
        L"WHATWOULDSHETHINK",
    };
    const int kTitleCount = (int)(sizeof(kTitles) / sizeof(kTitles[0]));

    // ------------------------------------------------------------ 状态 ----
    struct ChildData {
        int   variant = 0;      // 内容图序号 0-4，对应 RansomPopup1..5.png
        // （素材缺失时退回 face 的脸：0/1/2 分别闭口/张口/十字架）
        DWORD born = 0;
        DWORD life = 0;
    };
    HWND      g_driver = nullptr;
    HINSTANCE g_hInst = nullptr;
    HWND      g_main = nullptr;
    int       g_total = 0;

    struct Child {
        HWND      hwnd = nullptr;
        bool      closing = false;   // 正在淡出，等窗口销毁后回收
        bool      playerClosed = false;  // 玩家主动点关闭按钮关的（要扣倒计时）
        ChildData data;
    };
    // 用指针数组而不是对象数组：窗口回调持有 &data 的指针，
    // 如果存对象，vector::erase 搬移元素会让这些指针全部错位。
    std::vector<Child*> g_children;
        // 玩家主动关闭子窗口的次数。director 每帧用 ConsumePlayerClosedCount()
    // 消费它。
    //
    // 不用原子：回调、ReapDeadChildren、ConsumePlayerClosedCount 全都在
    // 主线程（驱动窗口的定时器回调）里跑，不存在并发。
    int g_playerClosedCount = 0;

    // aero 的 onUserClose 回调 —— 玩家点了子窗口的关闭按钮。
    // 这里**只置位**，真正的计数在 ReapDeadChildren 里做：等窗口动画
    // 真的走完、窗口销毁了才算数，免得动画还没放完就开始扣时间。
    void OnChildCloseClick(HWND /*hwnd*/, void* user)
    {
        Child* c = (Child*)user;
        if (!c) return;
        c->playerClosed = true;
        elog::Write(L"[popup] 玩家点了子窗口的关闭按钮（将扣 10 秒）");
    }

    // 主窗口状态
    int   g_gold = 0;
    int   g_goal = 500;
    DWORD g_remain = 0;
    DWORD g_totalMs = 0;         // 倒计时总长，用来算密度曲线

    // ------------------------------------------------------------ 密度曲线 ----
    // 0 = 刚开场，1 = 时间到。用它把间隔从 kGapMin/MaxMs 插值到 kGapMin/MaxEndMs。
    double Pressure()
    {
        if (g_totalMs == 0) return 0.0;
        double p = 1.0 - (double)g_remain / (double)g_totalMs;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        return p;
    }

    // 下一批子窗口隔多久。取 [lo, hi) 之间的随机值。
    DWORD NextWaveGap()
    {
        const double p = Pressure();

        // 注意：end 值比 start 值**小**（间隔是随时间收紧的），
        // 所以差值必须先转成 double 再算——用 DWORD 相减会无符号下溢成 42 亿，
        // 结果把「零星冒窗口」排到几天之后（实测日志打出「577110.7 秒后」）。
        const double lo = (double)kGapMinMs + ((double)kGapMinEndMs - (double)kGapMinMs) * p;
        const double hi = (double)kGapMaxMs + ((double)kGapMaxEndMs - (double)kGapMaxMs) * p;

        if (hi <= lo) return (DWORD)lo;
        return (DWORD)(lo + (double)(rand() % (int)(hi - lo)));
    }

    // 下一批冒几个。收尾阶段允许一次 3 个。
    int NextWaveCount()
    {
        const int span = (Pressure() > 0.66) ? 3 : 2;
        return 1 + (rand() % span);
    }

    DWORD g_nextWave = 0;        // 下一次冒子窗口的时间
    int   g_burstPending = 0;    // 开局批还没放出来的数量
    DWORD g_nextChildAt = 0;    // 下一个子窗口的放出时刻
    bool  g_burstDone = false;   // 开局那批是否已经清掉
    DWORD g_nextMainMove = 0;    // 主窗口下一次换位置的时刻

    // ------------------------------------------------------------ 主窗口挪位 ----
    // 在工作区里挑一个新位置，让 aero 沿弧线平滑移过去（不是瞬移）。
    void MoveMainRandomly()
    {
        if (!g_main || !aero::IsAlive(g_main)) return;

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

        const RECT mr = aero::RectOf(g_main);          // 含阴影的屏幕矩形
        const int w = mr.right - mr.left;
        const int h = mr.bottom - mr.top;

        // 内缩 kMainMoveMargin（现在是 0）：目标点仍保证**整窗落在工作区内**。
        const int x0 = wa.left + kMainMoveMargin;
        const int x1 = wa.right - w - kMainMoveMargin;
        const int y0 = wa.top + kMainMoveMargin;
        const int y1 = wa.bottom - h - kMainMoveMargin;
        if (x1 < x0 || y1 < y0) return;                // 工作区比窗口还小，放弃

        const int nx = x0 + rand() % (x1 - x0 + 1);
        const int ny = y0 + rand() % (y1 - y0 + 1);

        aero::AnimateMoveTo(g_main, nx, ny, kMainMoveDurMs);
        elog::Write(L"[popup] 主窗口换位置 -> (%d,%d)，%lums 弧线动画",
            nx, ny, (unsigned long)kMainMoveDurMs);
    }

    // ------------------------------------------------------------ 绘制 ----
    // 雪花：用 GDI+ 画（比 GDI 的 CreateSolidBrush 逐点快得多）
    void SprinkleNoise(Graphics& g, const RectF& rc, int count)
    {
        if (rc.Width < 4.0f || rc.Height < 4.0f) return;
        for (int i = 0; i < count; ++i)
        {
            const REAL x = rc.X + (REAL)(rand() % (int)rc.Width);
            const REAL y = rc.Y + (REAL)(rand() % (int)rc.Height);
            const BYTE v = (BYTE)(rand() & 0xFF);
            SolidBrush b(Color(190, v, v, v));
            g.FillRectangle(&b, x, y, 2.0f, 2.0f);
        }
    }

    void PaintChild(Graphics& g, const RectF& rc, DWORD frame, void* user)
    {
        const ChildData* d = (const ChildData*)user;
        if (!d) return;

        // 内容底板：半透明深色，跟玻璃窗融为一体
        SolidBrush bg(Color(150, 10, 12, 18));
        g.FillRectangle(&bg, rc);

        const RectF inner(rc.X + 6.0f, rc.Y + 6.0f, rc.Width - 12.0f, rc.Height - 12.0f);
        if (inner.Width < 16.0f || inner.Height < 16.0f) return;

        // 内容图交给 face 模块画。
        // 必须先 GetHDC 把 GDI+ 的绘制刷下去，再让 face 拿到裸 HDC。
        {
            HDC hdc = g.GetHDC();
            RECT r;
            r.left = (LONG)inner.X;
            r.top = (LONG)inner.Y;
            r.right = (LONG)(inner.X + inner.Width);
            r.bottom = (LONG)(inner.Y + inner.Height);

            // 首选 RansomPopup1..5.png，**拉伸铺满**整个内容区（不保持宽高比）。
            const int n = face::PopupImageCount();
            bool drew = (n > 0) && face::BlitPopupImage(hdc, r, d->variant % n);

            // 素材缺失就退回原来的脸 / 十字架，绝不开出一片空白
            if (!drew)
            {
                if (d->variant % 3 == 0) face::BlitFace(hdc, r, false, frame);
                else if (d->variant % 3 == 1) face::BlitFace(hdc, r, true, frame);
                else                          face::BlitCrucified(hdc, r, frame);
            }

            g.ReleaseHDC(hdc);
        }

        // 都叠一层轻雪花，做出信号不良的观感。
        // 点数比原来低：底下现在是有内容的图，画太密会把图糊掉。
        SprinkleNoise(g, inner, 70);
    }

    void PaintMain(Graphics& g, const RectF& rc, DWORD /*frame*/, void* /*user*/)
    {
        // 主窗口的内容**全部由 assets\main_window.ini 描述**，这里只做一件事：
        // 把当前的动态数值喂进去，然后交给 ui_layout 在内容区里渲染。
        // 想改排版（图放哪、字放哪、边框什么色）改那个 ini 就行，不用动代码。
        ui_layout::Status st;
        st.gold = g_gold;
        st.goal = (g_goal > 0) ? g_goal : 1;
        st.remainMs = g_remain;

        ui_layout::Render(g, rc, st);
    }

    // ------------------------------------------------------------ 子窗口 ----
    // lifeMs = 0 表示不自动消失，由调用方统一关闭（开局那批就是这种）
    void SpawnChild(DWORD lifeMs)
    {
        if ((int)g_children.size() >= kMaxChildren) return;

        Child* c = new Child();
        c->data.variant = rand() % 5;      // 对应 RansomPopup1..5.png
        c->data.born = GetTickCount();
        c->data.life = lifeMs;

        // 宽高**各自独立**随机，比例范围拉得很开（约 0.5 ~ 3.0）。
        // 同一张 RansomPopup 图被拉成完全不同的形状，
        // 每个子窗口看起来都不一样——这是故意的观感。
        const int w = 170 + rand() % 250;   // 170 - 419
        const int h = 140 + rand() % 190;   // 140 - 329

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int aw = wa.right - wa.left;
        const int ah = wa.bottom - wa.top;

        aero::Options opt;
        opt.title = kTitles[rand() % kTitleCount];
        opt.width = w;
        opt.height = h;
        opt.x = wa.left + rand() % (aw > w + 28 ? aw - w - 28 : 1);
        opt.y = wa.top + rand() % (ah > h + 28 ? ah - h - 28 : 1);
        opt.buttons = true;            // 子窗口有按钮，可以被一个个关掉
        opt.topmost = true;
        opt.resizable = false;         // 子窗口不做边缘缩放，免得玩的时候误拖
        opt.animate = true;            // 开关窗都走缩放淡入淡出

        // 玩家主动点关闭按钮 -> OnChildCloseClick -> 打标记 -> 稍后扣倒计时。
        // user 传的是 Child*（不是 &c->data —— paint 回调用的是 &c->data，
        // 两个 user 是分开的，互不干扰）。
        opt.onUserClose = &OnChildCloseClick;
        opt.onUserCloseUser = c;
        // 子窗口开得多（同时最多 14 个），重绘间隔要放宽，
        // 否则消息循环被重绘压满，窗口动画会被饿住。
        // 100ms 对雪花/故障这种内容来说反而更像「信号不良」。
        opt.tickMs = 130;

        c->hwnd = aero::Create(g_hInst, opt, PaintChild, &c->data);
        if (!c->hwnd) { delete c; return; }

        aero::SetWobble(c->hwnd, true);    // 子窗口原地轻微抖动 + 旋转

        elog::Write(L"[popup] 子窗口 %dx%d（比例 %.2f）图=RansomPopup%d",
            w, h, (double)w / (double)h, c->data.variant + 1);

        g_children.push_back(c);
        ++g_total;
    }

    // animate=true 时让它们各自淡出，窗口销毁后由 ReapDeadChildren 回收；
    // animate=false 时立即销毁（收尾/停止用，保证干净退出）。
    void CloseAllChildren(bool animate)
    {
        if (g_children.empty()) return;

        if (animate)
        {
            int n = 0;
            for (size_t i = 0; i < g_children.size(); ++i)
            {
                Child* c = g_children[i];
                if (c->closing) continue;
                c->closing = true;
                c->data.life = 0;              // 别再触发寿命回收
                aero::AnimateClose(c->hwnd);
                ++n;
            }
            if (n > 0) elog::Write(L"[popup] %d 个子窗口开始淡出", n);
            return;
        }

        std::vector<Child*> old = g_children;
        g_children.clear();

        for (size_t i = 0; i < old.size(); ++i)
        {
            aero::Destroy(old[i]->hwnd);
            delete old[i];
        }
        elog::Write(L"[popup] 立即关闭 %d 个子窗口", (int)old.size());
    }

    void ReapDeadChildren()
    {
        for (size_t i = 0; i < g_children.size(); )
        {
            if (!aero::IsAlive(g_children[i]->hwnd))
            {
                // 玩家**主动**关的（playerClosed）才计入扣时间；
                // 寿命到了自动淡出的不算。
                if (g_children[i]->playerClosed)
                    ++g_playerClosedCount;

                delete g_children[i];
                g_children.erase(g_children.begin() + i);
                continue;
            }
            ++i;
        }
    }

    void Tick()
    {
        const DWORD now = GetTickCount();

        // ---- 付钱演出推进 ----
        // 放在最前面：收场阶段 g_main 已经是空的了，不能走下面那条提前返回的路。
        if (g_payStage == PAY_TRAVEL && now - g_payStageAt >= kPayTravelMs)
        {
            // 到达中心：切换排版（背景 -> payup_bg），出场动画从头计时
            ui_layout::SetActive(1);
            audio::PlaySuccess();            // ransom_success —— 和 Accepta90 一起出来

            // Accepta90 冒出来的同时，整扇窗震一下。
            // 这是「赎金到账」的落点，给演出一个物理感，别让切排版那一刻太干。
            // 7px / 280ms：够明显，又不至于把刚飞到位、刚变好尺寸的窗口甩歪。
            // 若此刻 travel 动画还没收干净，aero 会把这次抖动排队，
            // 等它落位后再抖——窗口不会停在过冲位置上。
            if (aero::IsAlive(g_main))
                aero::AnimateJolt(g_main, 9.0f, 380);

            g_payStage = PAY_SHOW;
            g_payStageAt = now;
            elog::Write(L"[popup] 付钱演出：切到付钱排版，Accepta90 / Thankyou_sign 依次出场（窗口抖动）");
        }
        else if (g_payStage == PAY_SHOW && now - g_payStageAt >= kPayCloseAtMs)
        {
            // 收场：用**原本的关闭动画**（缩放 + 淡出，播完自己销毁）
            HWND w = g_main;
            g_main = nullptr;                // 交给 aero 自己销毁，别再碰这个句柄
            aero::AnimateClose(w);

            g_payStage = PAY_CLOSING;
            g_payStageAt = now;
            elog::Write(L"[popup] 付钱演出：播放关闭动画收场");
        }

        // 付钱演出期间，别再做任何「闹」的事——不挪窗口、不冒子窗口。
        // 少了这道闸，周期性的「主窗口换位置」会在演出中途把窗口
        // 从屏幕中心拖走，画面就散了。
        if (g_payStage != PAY_NONE)
        {
            ReapDeadChildren();
            return;
        }

        if (!g_main)
        {
            // 没有主窗口时也把残留子窗口收掉
            ReapDeadChildren();
            return;
        }

        ReapDeadChildren();

        // ---- 主窗口始终压在子窗口上面 ----
        SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // ---- 主窗口每隔一段时间换个位置（沿弧线平滑移动，不是瞬移）----
        if (now >= g_nextMainMove)
        {
            g_nextMainMove = now + kMainMoveMinMs +
                (DWORD)(rand() % (kMainMoveMaxMs - kMainMoveMinMs));
            MoveMainRandomly();
        }

        // ---- 开局批：错落放出 ----
        if (!g_burstDone && g_burstPending > 0 && now >= g_nextChildAt)
        {
            SpawnChild(0);                 // 0 = 活到整批关闭
            --g_burstPending;
            g_nextChildAt = now + 70;
        }

        // ---- 开局那批到点就一起淡出 ----
        if (!g_burstDone && now >= g_nextWave)
        {
            CloseAllChildren(true);
            g_burstDone = true;
            g_nextWave = now + NextWaveGap();
            elog::Write(L"[popup] 开局批开始关闭，%.1f 秒后开始零星冒窗口",
                (g_nextWave - now) / 1000.0);
            return;
        }

        // ---- 之后每隔一段时间冒 1-2 个，每个存活一段随机时间后自己消失 ----
        if (g_burstDone && now >= g_nextWave)
        {
            const int n = NextWaveCount();
            for (int i = 0; i < n; ++i)
            {
                const DWORD life = kChildLifeMin + (DWORD)(rand() % (kChildLifeMax - kChildLifeMin));
                SpawnChild(life);
            }
            elog::Write(L"[popup] 零星冒出 %d 个子窗口（存活 %lu-%lums）",
                n, (unsigned long)kChildLifeMin, (unsigned long)kChildLifeMax);

            g_nextWave = now + NextWaveGap();
        }

        // ---- 到寿命的子窗口淡出（窗口销毁后由 ReapDeadChildren 回收）----
        for (size_t i = 0; i < g_children.size(); ++i)
        {
            Child* c = g_children[i];
            if (c->closing) continue;
            if (aero::IsAlive(c->hwnd) && c->data.life > 0 &&
                now - c->data.born >= c->data.life)
            {
                c->closing = true;
                c->data.life = 0;
                aero::AnimateClose(c->hwnd);
            }
        }
    }

    LRESULT CALLBACK DriverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_TIMER:
            if (wp == kTickId) { Tick(); return 0; }
            break;
        case WM_ERASEBKGND: return 1;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

} // namespace

namespace popup {

    bool Start(HINSTANCE hInst)
    {
        if (g_driver) return true;
        g_hInst = hInst;

        // 两套排版：主勒索窗口 + 付完钱那个窗口。
        // 都在内嵌素材包里；取不到会退回内置默认排版 / 跳过付钱演出，
        // 绝不会开不出窗口。
        ui_layout::Load(L"main_window.ini", L"payup.ini");

        WNDCLASSEXW dc = { sizeof(WNDCLASSEXW) };
        dc.lpfnWndProc = DriverProc;
        dc.hInstance = hInst;
        dc.hCursor = nullptr;
        dc.hbrBackground = nullptr;
        dc.lpszClassName = kDriverClass;
        RegisterClassExW(&dc);

        g_driver = CreateWindowExW(0, kDriverClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, hInst, nullptr);
        if (!g_driver)
        {
            elog::Write(L"[popup] 驱动窗口创建失败, err=%lu", GetLastError());
            return false;
        }

        SetTimer(g_driver, kTickId, kTickMs, nullptr);
        elog::Write(L"[popup] 已就绪（%d 个标题，子窗口上限 %d）", kTitleCount, kMaxChildren);
        return true;
    }

    void Stop()
    {
        CloseAllChildren(false);
        if (g_main) { aero::Destroy(g_main); g_main = nullptr; }

        if (g_driver)
        {
            KillTimer(g_driver, kTickId);
            DestroyWindow(g_driver);
            g_driver = nullptr;
        }
        elog::Write(L"[popup] 已停止");
    }

    void BeginRansom(int childBurst)
    {
        if (!g_driver) return;

        EndRansom();

        // **一定要先切回主排版。**
        // 付完钱那套演出把 active 停在「付钱」排版上，不切回来的话，
        // 下一轮勒索开出来的会是付钱窗口（绿底 + Accepta90），而不是倒计时窗口。
        // SetActive 顺带会把 travel 复位。
        ui_layout::SetActive(0);

        // ---- 主勒索窗口：居中、无按钮、始终置顶 ----
        // 尺寸来自排版文件（assets\main_window.ini），不再是写死的常数。
        aero::Options opt;
        opt.title = L"RANSOM.exe";
        // 尺寸来自排版文件（assets\main_window.ini）。
        // 注意要过一道 OptionsWidth/HeightForContent：Options 的 height **含标题栏**，
        // 直接填内容尺寸的话画面会被纵向压扁。
        opt.width = aero::OptionsWidthForContent(ui_layout::Width());
        opt.height = aero::OptionsHeightForContent(ui_layout::Height());
        opt.buttons = false;          // 主窗口没有最大化/最小化/关闭
        opt.topmost = true;

        g_main = aero::Create(g_hInst, opt, PaintMain, nullptr);
        if (!g_main) elog::Write(L"[popup] 主窗口创建失败");
        else         aero::SetWobble(g_main, true);   // 主窗口也原地轻微抖动

        // ---- 开局那批子窗口 ----
        if (childBurst < 1)            childBurst = 1;
        if (childBurst > kMaxChildren) childBurst = kMaxChildren;

        g_total = 0;
        g_children.reserve(kMaxChildren);

        // 开局批**错落**放出（每 70ms 一个）。
        // 一次性建 9 个窗口会把消息循环堵住，每个窗口自己的打开动画定时器
        // 都会被推迟，结果看起来就像瞬间出现、完全没有弹出感。
        g_burstPending = childBurst;
        g_nextChildAt = GetTickCount();

        g_burstDone = false;
        g_nextWave = GetTickCount() + kBurstLifeMs;

        // 第一次换位置：等开局那批子窗口散掉之后再动，别一开场就晃
        g_nextMainMove = GetTickCount() + kBurstLifeMs + 2000;

        elog::Write(L"[popup] 勒索开始：主窗口=%s，开局批 %d 个错落放出（%lu 秒后统一关闭）",
            g_main ? L"成功" : L"失败", childBurst,
            (unsigned long)(kBurstLifeMs / 1000));
    }

    void EndRansom()
    {
        CloseAllChildren(false);
        if (g_main)
        {
            // 正在给付钱演出收尾（关闭动画已经在跑了）就别再硬销毁一次
            if (g_payStage != PAY_CLOSING) aero::Destroy(g_main);
            g_main = nullptr;
        }
        g_payStage = PAY_NONE;
        g_burstDone = false;
        g_nextWave = 0;
        // 清掉上一轮的倒计时：totalMs=0 时 Pressure() 返回 0，
        // 也就是回到最宽松的节奏，不会把上一轮末尾的紧张感带过来。
        g_remain = 0;
        g_totalMs = 0;
    }

    // ---- 付完钱：开始那套演出 ----
    void BeginPayup()
    {
        if (!g_main || !aero::IsAlive(g_main)) return;

        // 付钱排版没准备好就什么都不做：主窗口留在原地，
        // 由 director 那边的 EndRansom 照常收掉，不会卡住。
        if (!ui_layout::PayupReady())
        {
            elog::Write(L"[popup] 付钱排版不可用，跳过付钱演出");
            return;
        }

        CloseAllChildren(false);        // 子窗口先退场
        g_burstDone = true;             // 开局批的定时器一并停掉，别在演出期间再触发
        g_nextWave = 0;

        // 演出期间**关掉原地抖动**：付钱窗口要稳稳地停在屏幕中心，
        // 不能还带着 ±1px 位移 + ±0.3° 旋转（那是勒索阶段的效果）。
        // 下一轮 BeginRansom 会重新打开。
        aero::SetWobble(g_main, false);

        // 1) 只留 A90 的头 + 强制黑底
        ui_layout::SetTravel(true);
        aero::Repaint(g_main);

        // 2) 飞到屏幕正中央，**同时**缩放到付钱窗口的内容区大小
        //    （同样要过 OptionsWidth/HeightForContent，否则付钱排版会被压扁）
        //    走的是分帧 + 过冲版本：10 帧跳过去，中途冲过中心约 12% 再弹回来。
        const int sw = aero::ShadowSize() * 2;
        const int w = aero::OptionsWidthForContent(ui_layout::PayupWidth()) + sw;
        const int h = aero::OptionsHeightForContent(ui_layout::PayupHeight()) + sw;

        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const int x = wa.left + ((wa.right - wa.left) - w) / 2;
        const int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

        // arc=false：过冲和弧线叠加会把落点甩歪，这里走直线更干净。
        aero::AnimateMoveToStepped(g_main, x, y, kPayTravelMs, w, h,
            kPayTravelFrames, true, false);

        g_payStage = PAY_TRAVEL;
        g_payStageAt = GetTickCount();

        elog::Write(L"[popup] 付钱演出开始：飞向屏幕中心 (%d,%d) 并缩放到 %dx%d"
            L"（%lums / %d 帧 / 过冲）",
            x, y, w, h, (unsigned long)kPayTravelMs, kPayTravelFrames);
    }

    void SetStatus(int gold, int goal, DWORD remainMs, DWORD totalMs)
    {
        g_gold = gold;
        g_goal = (goal > 0) ? goal : 1;
        g_remain = remainMs;
        g_totalMs = totalMs;
    }
    bool MainAlive() { return aero::IsAlive(g_main); }
    int  Children() { return (int)g_children.size(); }
    int  TotalSpawned() { return g_total; }

    int ConsumePlayerClosedCount()
    {
        const int n = g_playerClosedCount;
        g_playerClosedCount = 0;
        return n;
    }
} // namespace popup