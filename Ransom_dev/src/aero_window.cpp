// ============================================================================
//  aero_window.cpp
//
//  绘制流程与原版一致：先在 32bpp DIB 上用 GDI+ 画好整扇窗（阴影 / 圆角 /
//  标题栏高光 / 底部反光），把内容交给回调，手动预乘 alpha，
//  最后 UpdateLayeredWindow 呈现。
//
//  动画也按原版恢复：
//    打开    缩放 0.85 + alpha 120 -> 1.0 / 255      （200ms，缓出）
//    关闭    缩放 -> 0.90 + 淡出                      （180ms，缓入）
//    最大化  窗口矩形插值到工作区                      （220ms，缓出）
//    还原    矩形插值回原位置                          （220ms，缓出）
//    最小化  缩放 -> 0.35 + 淡出，锚点在底部            （200ms，缓出）
//    换位置  沿弧线平移到新的随机位置                    （900ms，smoothstep）
//    原地抖动 真实窗口位移做衰减振荡                    （时长由调用方给）
//  另有：原地轻微抖动 & 旋转（角点位移 < 3px，纯绘制层实现，不动窗口矩形）、
//        边缘拖拽缩放、右键/Alt+Space 系统菜单、双击标题最大化、F11。
// ============================================================================
#include "aero_window.h"

#include "entity_log.h"

#include <windowsx.h>      // GET_X_LPARAM / GET_Y_LPARAM
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")

namespace {

    const wchar_t* kClassName = L"RansomAeroWnd";

    // 与原版一致的观感参数
    const int kCorner = 8;
    const int kShadow = 14;
    const int kTitleH = 32;
    const int kBtnW = 34;
    const int kBtnH = 22;
    const int kBtnGap = 2;
    const int kBtnRight = 10;
    const int kResizeHot = 10;

    const int kMinContentW = 200;
    const int kMinContentH = 120;

    const UINT_PTR kTickId = 1;

    // ---- 原地抖动 ----
    // 「稍微抖动 & 旋转」：随机位移动 + 绕中心旋转，幅度都刻意压得很小。
    //
    // 位移和旋转是**叠加**的，最坏情况下的角点位移 =
    //     位移 * √2 + 半对角线 * sin(旋转角)
    // 主勒索窗口内容区 400x250，半对角线约 236px，于是：
    //     1.0 * 1.414 + 236 * sin(0.30°) ≈ 1.41 + 1.24 ≈ 2.65px   （< 3px ✓）
    //
    // 三个不同的周期（约 0.9 / 1.3 / 1.7 秒）让 x、y、角度各走各的，
    // 合起来是李萨如式的漂移，不会所有窗口整齐划一地晃。
    //
    // 因为窗口外层本来就有 kShadow=14px 的透明边距，这点位移和旋转
    // **不会被裁掉**，所以完全不用动窗口矩形——纯绘制层的事，
    // 也不影响鼠标命中判定和拖拽。
    const float kWobblePx = 1.0f;      // 位移振幅（px）
    const float kWobbleDeg = 0.30f;     // 旋转振幅（度）
    const double kFreqX = 0.00698;   // 2π/900ms
    const double kFreqY = 0.00483;   // 2π/1300ms
    const double kFreqRot = 0.00370;   // 2π/1700ms

    // ---- 动画参数（照搬原版）----
    namespace animcfg {
        const UINT  kAnimMs = 15;    // 动画期间的重绘间隔
        const UINT  kIdleMs = 33;    // 静止时的重绘间隔（内容动画用）

        const DWORD kOpenMs = 300;
        const DWORD kCloseMs = 180;
        const DWORD kMaximizeMs = 220;
        const DWORD kRestoreMs = 220;
        const DWORD kMinimizeMs = 200;
        const DWORD kTaskbarMs = 240;

        const float kOpenScale = 0.78f;
        const BYTE  kOpenAlpha = 110;
        const float kCloseScale = 0.90f;
        const float kMinScale = 0.35f;
        const float kTaskbarScale = 0.30f;
    }

    enum { BTN_NONE = 0, BTN_MIN, BTN_MAX, BTN_CLOSE };

    enum AnimType {
        ANIM_NONE = 0,
        ANIM_OPEN,
        ANIM_CLOSE,
        ANIM_MAXIMIZE,
        ANIM_RESTORE,
        ANIM_MINIMIZE,
        ANIM_TASKBAR_RESTORE,
        ANIM_MOVE,           // 平移到新位置，沿弧线走
        ANIM_JOLT            // 原地抖一下（真实窗口位移，衰减振荡）
    };

    struct AnimState {
        AnimType type = ANIM_NONE;
        bool     active = false;
        DWORD    startMs = 0;
        DWORD    durMs = 0;

        RECT fromRect{}, toRect{};
        bool useRect = false;

        float fromScale = 1.0f, toScale = 1.0f;
        bool  useScale = false;

        BYTE fromAlpha = 255, toAlpha = 255;

        float anchorX = 0.5f, anchorY = 0.5f;

        float arcAmp = 0.0f;     // ANIM_MOVE：垂直于位移方向的弧高（px），可正可负

        float joltAmp = 0.0f;    // ANIM_JOLT：初始振幅（px），随时间平方衰减到 0

        RECT  bounds{};          // ANIM_MOVE：动画期间允许的活动范围（工作区）
        bool  useBounds = false; // 弧线会把窗口甩出去，所以每帧要夹回范围内

        // ---- 分帧移动（"卡帧"手感）----
        // steps > 0 时，整段动画只走 steps 个离散位置：t 被量化成 k/steps，
        // 窗口一格一格地跳，而不是平滑滑动。steps == 0 就是原来的连续插值。
        int   steps = 0;
        // 缓动换成 back-out（冲过目标再弹回来），配合分帧就是「一下甩到位」。
        bool  overshoot = false;
    };

    struct AeroWnd {
        HWND      hwnd = nullptr;
        aero::Options opt;
        aero::PaintFn paint = nullptr;
        void* user = nullptr;

        int  w = 0, h = 0;             // 含阴影
        bool maximized = false;
        bool minimized = false;
        RECT restoreRect{};
        RECT preMinimizeRect{};

        RECT rcMin{}, rcMax{}, rcClose{};
        int  hot = BTN_NONE;
        bool active = true;

        AnimState anim;

        bool      wobble = false;   // 原地抖动 & 旋转
        float     wobbleSeed = 0.0f;    // 每个窗口一个相位，免得所有窗口同步晃

        // AnimateJolt 撞上别的动画时的排队位（见 StartJolt / OnAnimTick 末尾）
        bool  joltQueued = false;
        float joltQueuedAmp = 0.0f;
        DWORD joltQueuedMs = 0;

        HDC       memDC = nullptr;
        HBITMAP   bmp = nullptr;
        HGDIOBJ   oldBmp = nullptr;
        void* bits = nullptr;

        DWORD     frame = 0;
    };

    std::vector<AeroWnd*> g_windows;

    // ---- 外观资源（图标 / 字体），由 aero::SetAppIconFromSelf / SetTitleFont* 填 ----
    // exe 图标资源的 ID（Ransom_dev.rc 里的 `1 ICON`）
    const int kAppIconResId = 1;

    HICON        g_iconBig = nullptr;
    HICON        g_iconSmall = nullptr;
    // GDI+ 的 Graphics **没有** DrawIcon（那是 GDI 的东西），
    // 所以标题栏那个图标要先把 HICON 转成 Bitmap 缓存下来，别再每帧转一次。
    Gdiplus::Bitmap* g_iconBmp = nullptr;

    // UI 字体。**不能用 FontFamily(L"名字") 去查**：AddFontResourceEx(FR_PRIVATE)
    // 注册的字体，GDI+ 按名字是查不到的（实测 GetLastStatus != Ok，会静默回退，
    // 用户会以为字体没生效）。必须走 PrivateFontCollection，拿到 FontFamily 再用。
    //
    // g_fontBytes 是字体文件的字节。素材现在内嵌在 exe 里，
    // AddMemoryFont / AddFontMemResourceEx 都**不拷贝**这块内存，
    // 所以必须自己留一份，一直活到进程结束。
    std::vector<unsigned char>       g_fontBytes;
    Gdiplus::PrivateFontCollection* g_pfc = nullptr;
    Gdiplus::FontFamily* g_titleFamily = nullptr;

    // 标题栏字体。注册成功走素材里那套，否则退回系统雅黑。
    Gdiplus::Font MakeTitleFont()
    {
        if (g_titleFamily) return Gdiplus::Font(g_titleFamily, 15.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        return Gdiplus::Font(L"Microsoft YaHei", 15.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    }

    AeroWnd* From(HWND hwnd) { return (AeroWnd*)GetWindowLongPtrW(hwnd, GWLP_USERDATA); }

    // ------------------------------------------------------------ 绘图工具 ----
    void AddRoundRect(GraphicsPath& path, const RectF& r, REAL radius)
    {
        REAL d = radius * 2.0f;
        if (d > r.Width)  d = r.Width;
        if (d > r.Height) d = r.Height;
        if (d < 0) d = 0;

        path.Reset();
        path.StartFigure();
        path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
        path.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
        path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
        path.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void Premultiply(void* bits, int w, int h)
    {
        BYTE* p = (BYTE*)bits;
        const size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; ++i, p += 4)
        {
            const BYTE a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (BYTE)(p[0] * a / 255);
            p[1] = (BYTE)(p[1] * a / 255);
            p[2] = (BYTE)(p[2] * a / 255);
        }
    }

    void DrawCaptionButton(Graphics& g, const RECT& rc, int id, bool hot, bool maximized)
    {
        const RectF rf((REAL)rc.left, (REAL)rc.top,
            (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));
        const REAL rad = 3.0f;

        GraphicsPath p;
        AddRoundRect(p, rf, rad);

        const Color bg = (id == BTN_CLOSE && hot)
            ? Color(220, 210, 60, 50)
            : (hot ? Color(200, 200, 220, 240) : Color(20, 255, 255, 255));

        SolidBrush br(bg);
        g.FillPath(&br, &p);

        Pen pen(Color(120, 90, 130, 180), 1.0f);
        g.DrawPath(&pen, &p);

        const bool isClose = (id == BTN_CLOSE);
        const Color symColor = (isClose && hot) ? Color(255, 255, 255, 255)
            : Color(240, 240, 245, 255);
        Pen sp(symColor, 1.5f);
        sp.SetLineCap(LineCapRound, LineCapRound, DashCapRound);

        const REAL cx = rf.X + rf.Width * 0.5f;
        const REAL cy = rf.Y + rf.Height * 0.5f;

        switch (id)
        {
        case BTN_MIN:
            g.DrawLine(&sp, cx - 5.0f, cy + 4.0f, cx + 5.0f, cy + 4.0f);
            break;

        case BTN_MAX:
            if (maximized)
            {
                g.DrawRectangle(&sp, cx - 5.0f, cy - 6.0f, 8.0f, 8.0f);
                g.DrawLine(&sp, cx - 3.0f, cy - 4.0f, cx + 5.0f, cy - 4.0f);
                g.DrawLine(&sp, cx + 5.0f, cy - 4.0f, cx + 5.0f, cy + 4.0f);
                g.DrawLine(&sp, cx + 5.0f, cy + 4.0f, cx - 1.0f, cy + 4.0f);
            }
            else
            {
                g.DrawRectangle(&sp, cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            }
            break;

        case BTN_CLOSE:
            g.DrawLine(&sp, cx - 5.0f, cy - 5.0f, cx + 5.0f, cy + 5.0f);
            g.DrawLine(&sp, cx + 5.0f, cy - 5.0f, cx - 5.0f, cy + 5.0f);
            break;

        default: break;
        }
    }

    // ------------------------------------------------------------ 后备缓冲 ----
    bool EnsureBuffer(AeroWnd* a, int w, int h)
    {
        if (a->memDC && a->bits && a->w == w && a->h == h) return true;

        if (a->memDC && a->oldBmp) { SelectObject(a->memDC, a->oldBmp); a->oldBmp = nullptr; }
        if (a->bmp) { DeleteObject(a->bmp); a->bmp = nullptr; }
        if (a->memDC) { DeleteDC(a->memDC);   a->memDC = nullptr; }
        a->bits = nullptr;

        HDC screen = GetDC(nullptr);
        a->memDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!a->memDC) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        a->bmp = CreateDIBSection(a->memDC, &bi, DIB_RGB_COLORS, &a->bits, nullptr, 0);
        if (!a->bmp || !a->bits) return false;

        a->oldBmp = SelectObject(a->memDC, a->bmp);
        a->w = w; a->h = h;
        return true;
    }

    // ------------------------------------------------------------ 绘制 ----
    void PaintWindow(AeroWnd* a,
        float scale = 1.0f,
        BYTE alphaOverride = 255,
        float anchorX = 0.5f,
        float anchorY = 0.5f)
    {
        if (!a || !a->memDC) return;

        const int W = a->w, H = a->h;
        if (W <= 0 || H <= 0) return;

        Graphics g(a->memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.Clear(Color(0, 0, 0, 0));

        // ---- 原地抖动：绕中心旋转 + 平移 ----
        // 只在静止时抖（动画期间窗口本来就在动，再叠一层没必要）。
        // 相位用 double 算：GetTickCount() 到几十万毫秒时，float 的精度
        // 已经不够表示 sin 的参数了，会出现明显的台阶。
        double wobPx = 0.0, wobPy = 0.0, wobDeg = 0.0;
        if (a->wobble && !a->anim.active)
        {
            const double t = (double)GetTickCount();
            const double s = (double)a->wobbleSeed;
            wobPx = (double)kWobblePx * sin(t * kFreqX + s);
            wobPy = (double)kWobblePx * sin(t * kFreqY + s * 1.7);
            wobDeg = (double)kWobbleDeg * sin(t * kFreqRot + s * 0.3);
        }

        // ---- 变换：先绕锚点缩放，再叠加抖动的旋转与平移 ----
        if (scale != 1.0f || wobPx != 0.0 || wobPy != 0.0 || wobDeg != 0.0)
        {
            const REAL cx = W * anchorX;
            const REAL cy = H * anchorY;
            g.TranslateTransform(cx, cy);
            if (wobDeg != 0.0) g.RotateTransform((REAL)wobDeg);
            if (scale != 1.0f) g.ScaleTransform(scale, scale);
            g.TranslateTransform(-cx + (REAL)wobPx, -cy + (REAL)wobPy);
        }

        const REAL S = (REAL)kShadow;
        const RectF rcMain(S, S, (REAL)(W - kShadow * 2), (REAL)(H - kShadow * 2));

        GraphicsPath mainPath;
        AddRoundRect(mainPath, rcMain, (REAL)kCorner);

        // 1. 四周阴影
        {
            GraphicsPath inner;
            AddRoundRect(inner, rcMain, (REAL)kCorner);
            g.SetClip(&inner, CombineModeExclude);

            for (int i = kShadow; i >= 1; --i)
            {
                const RectF r(rcMain.X - i, rcMain.Y - i,
                    rcMain.Width + i * 2.0f, rcMain.Height + i * 2.0f);
                GraphicsPath p;
                AddRoundRect(p, r, (REAL)kCorner + i);

                const BYTE al = (BYTE)(2 + (kShadow - i) * 3 / 4);
                SolidBrush br(Color(al, 0, 10, 25));
                g.FillPath(&br, &p);
            }
            g.ResetClip();
        }

        // 2. 主体底色
        {
            SolidBrush br(Color(1, 255, 255, 255));
            g.FillPath(&br, &mainPath);
        }

        // 3. 标题栏高光
        {
            g.SetClip(&mainPath);
            const RectF rcTitle(rcMain.X, rcMain.Y, rcMain.Width, (REAL)kTitleH);

            const BYTE baseTop = a->active ? 190 : 90;
            const BYTE baseMid = a->active ? 140 : 60;
            const BYTE baseLower = a->active ? 60 : 25;

            const Color cols[] = {
                Color(baseTop,   255, 255, 255),
                Color(baseMid,   255, 255, 255),
                Color(baseLower, 255, 255, 255),
                Color(0,         255, 255, 255),
            };
            const REAL stops[] = { 0.0f, 0.25f, 0.60f, 1.0f };

            for (int i = 0; i < 3; ++i)
            {
                const REAL y0 = rcTitle.Y + rcTitle.Height * stops[i];
                const REAL y1 = rcTitle.Y + rcTitle.Height * stops[i + 1];
                LinearGradientBrush br(PointF(rcTitle.X, y0), PointF(rcTitle.X, y1),
                    cols[i], cols[i + 1]);
                g.FillRectangle(&br, rcTitle.X, y0, rcTitle.Width, y1 - y0);
            }

            {
                const BYTE a0 = a->active ? 230 : 120;
                const BYTE a1 = a->active ? 60 : 25;
                const RectF rcEdge(rcMain.X + 2.0f, rcMain.Y + 1.0f, rcMain.Width - 4.0f, 1.5f);
                LinearGradientBrush br(PointF(rcEdge.X, rcEdge.Y),
                    PointF(rcEdge.X, rcEdge.GetBottom()),
                    Color(a0, 255, 255, 255), Color(a1, 255, 255, 255));
                g.FillRectangle(&br, rcEdge);
            }
            g.ResetClip();
        }

        // 4. 底部内阴影
        {
            g.SetClip(&mainPath);
            const REAL shadowH = 28.0f;
            const RectF rcShadow(rcMain.X, rcMain.GetBottom() - shadowH, rcMain.Width, shadowH);
            LinearGradientBrush br(PointF(rcShadow.X, rcShadow.Y),
                PointF(rcShadow.X, rcShadow.GetBottom()),
                Color(0, 0, 0, 0), Color(20, 0, 0, 0));
            g.FillRectangle(&br, rcShadow);
            g.ResetClip();
        }

        // 5. 底部反光
        {
            g.SetClip(&mainPath);
            const REAL reflectH = 34.0f;
            const REAL overhang = 40.0f;
            const RectF rcReflect(rcMain.X - overhang, rcMain.GetBottom() - reflectH,
                rcMain.Width + overhang * 2.0f, reflectH);

            const Color cols[] = {
                Color(0,   210, 230, 255),
                Color(110, 220, 240, 255),
                Color(210, 235, 248, 255),
            };
            const REAL stops[] = { 0.0f, 0.55f, 1.0f };
            for (int i = 0; i < 2; ++i)
            {
                const REAL y0 = rcReflect.Y + rcReflect.Height * stops[i];
                const REAL y1 = rcReflect.Y + rcReflect.Height * stops[i + 1];
                LinearGradientBrush br(PointF(rcReflect.X, y0), PointF(rcReflect.X, y1),
                    cols[i], cols[i + 1]);
                g.FillRectangle(&br, rcReflect.X, y0, rcReflect.Width, y1 - y0);
            }
            g.ResetClip();
        }

        // 6. 描边
        {
            Pen pen(Color(200, 90, 130, 200), 1.5f);
            g.DrawPath(&pen, &mainPath);
        }

        // 6b. 底边白亮光
        {
            g.SetClip(&mainPath);
            const REAL overhang = 40.0f;
            const RectF rcLine(rcMain.X - overhang, rcMain.GetBottom() - 2.0f,
                rcMain.Width + overhang * 2.0f, 1.8f);
            LinearGradientBrush br(PointF(rcLine.X, rcLine.Y),
                PointF(rcLine.X, rcLine.GetBottom()),
                Color(60, 255, 255, 255), Color(230, 255, 255, 255));
            g.FillRectangle(&br, rcLine);
            g.ResetClip();
        }

        // 7. 标题栏分隔线
        {
            g.SetClip(&mainPath);
            const REAL y = rcMain.Y + (REAL)kTitleH;
            Pen pen(Color(150, 120, 150, 200), 1.0f);
            g.DrawLine(&pen, rcMain.X + 1.0f, y, rcMain.GetRight() - 1.0f, y);
            g.ResetClip();
        }

        // 8. 标题栏图标 + 标题文字
        {
            // 右边留给三个按钮，图标占最左边，文字相应右移
            const REAL textRight = rcMain.GetRight() - 156.0f;
            REAL textX = rcMain.X + 14.0f;

            if (g_iconBmp)
            {
                const REAL isz = 16.0f;
                const REAL iy = rcMain.Y + ((REAL)kTitleH - isz) * 0.5f;
                g.DrawImage(g_iconBmp,
                    RectF(rcMain.X + 8.0f, iy, isz, isz),
                    0.0f, 0.0f,
                    (REAL)g_iconBmp->GetWidth(), (REAL)g_iconBmp->GetHeight(),
                    UnitPixel);
                textX = rcMain.X + 8.0f + isz + 8.0f;
            }

            Font font = MakeTitleFont();
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            sf.SetTrimming(StringTrimmingEllipsisCharacter);
            sf.SetFormatFlags(StringFormatFlagsNoWrap);

            RectF rcText(textX, rcMain.Y, textRight - textX, (REAL)kTitleH);

            {
                SolidBrush sh(Color(200, 10, 30, 60));
                RectF r2 = rcText; r2.Y += 1.0f;
                g.DrawString(a->opt.title.c_str(), -1, &font, r2, &sf, &sh);
            }
            {
                const BYTE ta = a->active ? 255 : 160;
                SolidBrush tb(Color(ta, 245, 250, 255));
                g.DrawString(a->opt.title.c_str(), -1, &font, rcText, &sf, &tb);
            }
        }

        // 9. 标题栏按钮。主窗口 buttons=false —— 一个按钮都不画
        if (a->opt.buttons)
        {
            const int top = (int)(rcMain.Y + (kTitleH - kBtnH) * 0.5f);
            const int right = (int)rcMain.GetRight() - kBtnRight;

            SetRect(&a->rcClose, right - kBtnW, top, right, top + kBtnH);
            SetRect(&a->rcMax, right - kBtnW * 2 - kBtnGap, top,
                right - kBtnW - kBtnGap, top + kBtnH);
            SetRect(&a->rcMin, right - kBtnW * 3 - kBtnGap * 2, top,
                right - kBtnW * 2 - kBtnGap * 2, top + kBtnH);

            DrawCaptionButton(g, a->rcMin, BTN_MIN, a->hot == BTN_MIN, a->maximized);
            DrawCaptionButton(g, a->rcMax, BTN_MAX, a->hot == BTN_MAX, a->maximized);
            DrawCaptionButton(g, a->rcClose, BTN_CLOSE, a->hot == BTN_CLOSE, a->maximized);
        }
        else
        {
            SetRectEmpty(&a->rcMin);
            SetRectEmpty(&a->rcMax);
            SetRectEmpty(&a->rcClose);
        }

        // 10. 内容交给回调
        if (a->paint)
        {
            const RectF content(rcMain.X + 1.0f,
                rcMain.Y + (REAL)kTitleH + 1.0f,
                rcMain.Width - 2.0f,
                rcMain.Height - (REAL)kTitleH - 2.0f);
            a->paint(g, content, a->frame, a->user);
        }

        // ---- 预乘 alpha 后呈现 ----
        Premultiply(a->bits, W, H);

        HDC screen = GetDC(nullptr);
        BLENDFUNCTION bf;
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = alphaOverride;
        bf.AlphaFormat = AC_SRC_ALPHA;

        SIZE  size = { W, H };
        POINT src = { 0, 0 };
        UpdateLayeredWindow(a->hwnd, screen, nullptr, &size, a->memDC, &src, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, screen);
    }

    // ------------------------------------------------------------ 动画 ----
    void SetTickRate(AeroWnd* a, UINT ms)
    {
        SetTimer(a->hwnd, kTickId, ms, nullptr);
    }

    void StartAnim(AeroWnd* a, AnimType type, DWORD durMs);
    void StartMove(AeroWnd* a, int x, int y, int w, int h, DWORD durMs,
        int steps = 0, bool overshoot = false, bool arc = true);
    void StartJolt(AeroWnd* a, float amp, DWORD durMs);

    void CancelAnim(AeroWnd* a)
    {
        if (a->anim.active)
        {
            a->anim.active = false;
            a->anim.type = ANIM_NONE;
            a->anim.useRect = false;
            a->anim.useScale = false;
            a->anim.arcAmp = 0.0f;
            a->anim.joltAmp = 0.0f;
            a->anim.useBounds = false;
            a->anim.steps = 0;
            a->anim.overshoot = false;
        }
        SetTickRate(a, a->opt.tickMs);
    }

    // 平移到新位置，走一条弧线。w/h 传 0 表示保持当前尺寸。
    // x/y 是**含阴影的**窗口左上角屏幕坐标（和 RectOf() 同一套坐标）。
    // steps/overshoot/arc 见 AnimState 上的注释。
    void StartMove(AeroWnd* a, int x, int y, int w, int h, DWORD durMs,
        int steps, bool overshoot, bool arc)
    {
        if (a->anim.active || a->minimized || a->maximized) return;

        RECT cur; GetWindowRect(a->hwnd, &cur);
        const int curW = cur.right - cur.left;
        const int curH = cur.bottom - cur.top;
        if (w <= 0) w = curW;
        if (h <= 0) h = curH;
        if (cur.left == x && cur.top == y && curW == w && curH == h) return;

        // 弧高按位移长度取，有下限免得短距离看不出弧，有上限免得飞出屏幕。
        // 方向随机，这样每次换位置的走法都不一样。
        // arc=false 时弧高留 0，走直线。
        const float dx = (float)(x - cur.left);
        const float dy = (float)(y - cur.top);
        const float len = sqrtf(dx * dx + dy * dy);
        float arcAmp = 0.0f;
        if (arc)
        {
            arcAmp = len * 0.15f;
            if (arcAmp > 60.0f) arcAmp = 60.0f;
            if (arcAmp < 12.0f) arcAmp = 12.0f;
            if (rand() & 1) arcAmp = -arcAmp;
        }

        a->anim.useRect = true;
        a->anim.useScale = false;
        a->anim.fromRect = cur;
        SetRect(&a->anim.toRect, x, y, x + w, y + h);
        a->anim.arcAmp = arcAmp;
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 255;
        a->anim.steps = (steps > 0) ? steps : 0;
        a->anim.overshoot = overshoot;

        // 弧线在中途会把窗口甩到位移方向的两侧，终点没出界不代表中途也没出界。
        // 所以记下所在显示器的工作区，OnAnimTick 每帧把位置夹回去——
        // 贴边时弧线会被压平，但**绝不会跑出屏幕**。
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(MonitorFromWindow(a->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        a->anim.bounds = mi.rcWork;
        a->anim.useBounds = true;

        StartAnim(a, ANIM_MOVE, durMs);
    }

    // 原地抖一下：真实窗口位移，振幅按 (1-t)^2 衰减，最后一帧精确回原位。
    // 刻意用**真实位移**而不是绘制层偏移——这一下要的就是整扇窗被撞了一下的
    // 物理感，绘制层那点幅度（±1px）压不住这个场合。
    void StartJolt(AeroWnd* a, float amp, DWORD durMs)
    {
        if (!a || amp <= 0.0f) return;
        if (a->minimized || a->maximized) return;
        if (durMs < 60) durMs = 60;

        // 正在跑别的动画：排队，等它在 OnAnimTick 末尾收干净再抖。
        // 这里**不能**强插：travel 那种带 back-out 过冲的移动被中途掐掉，
        // 窗口会停在过冲位置上，抖完就偏几像素、回不到终点。
        if (a->anim.active)
        {
            a->joltQueued = true;
            a->joltQueuedAmp = amp;
            a->joltQueuedMs = durMs;
            return;
        }

        RECT cur;
        GetWindowRect(a->hwnd, &cur);

        a->anim.useRect = false;      // 位移由本类型自己在 OnAnimTick 里做
        a->anim.useScale = false;
        a->anim.fromRect = cur;
        a->anim.toRect = cur;        // 不移动，只抖
        a->anim.joltAmp = amp;
        a->anim.arcAmp = 0.0f;
        a->anim.useBounds = false;
        a->anim.steps = 0;
        a->anim.overshoot = false;
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 255;

        StartAnim(a, ANIM_JOLT, durMs);
    }

    void StartAnim(AeroWnd* a, AnimType type, DWORD durMs)
    {
        a->anim.type = type;
        a->anim.active = true;
        a->anim.startMs = GetTickCount();
        a->anim.durMs = durMs;
        SetTickRate(a, animcfg::kAnimMs);
    }

    RECT MaximizeTarget(AeroWnd* a)
    {
        MONITORINFO mi = { sizeof(MONITORINFO) };
        GetMonitorInfoW(MonitorFromWindow(a->hwnd, MONITOR_DEFAULTTONEAREST), &mi);

        RECT r = mi.rcWork;
        InflateRect(&r, -kShadow, -kShadow);      // 阴影内缩，避免被屏幕边缘裁掉
        return r;
    }

    void StartOpen(AeroWnd* a)
    {
        a->anim.useRect = false;
        a->anim.useScale = true;
        a->anim.fromScale = animcfg::kOpenScale;
        a->anim.toScale = 1.0f;
        a->anim.fromAlpha = animcfg::kOpenAlpha;
        a->anim.toAlpha = 255;
        a->anim.anchorX = 0.5f;
        a->anim.anchorY = 0.5f;
        StartAnim(a, ANIM_OPEN, animcfg::kOpenMs);
    }

    void StartClose(AeroWnd* a)
    {
        CancelAnim(a);
        a->anim.useRect = false;
        a->anim.useScale = true;
        a->anim.fromScale = 1.0f;
        a->anim.toScale = animcfg::kCloseScale;
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 0;
        a->anim.anchorX = 0.5f;
        a->anim.anchorY = 0.5f;
        StartAnim(a, ANIM_CLOSE, animcfg::kCloseMs);
    }

    void StartMaximize(AeroWnd* a)
    {
        if (a->anim.active || a->maximized || a->minimized) return;

        RECT cur; GetWindowRect(a->hwnd, &cur);
        a->restoreRect = cur;

        a->anim.useRect = true;
        a->anim.useScale = false;
        a->anim.fromRect = cur;
        a->anim.toRect = MaximizeTarget(a);
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 255;

        a->maximized = true;                 // 状态先行
        StartAnim(a, ANIM_MAXIMIZE, animcfg::kMaximizeMs);
    }

    void StartRestore(AeroWnd* a)
    {
        if (a->anim.active || !a->maximized) return;

        RECT cur; GetWindowRect(a->hwnd, &cur);

        a->anim.useRect = true;
        a->anim.useScale = false;
        a->anim.fromRect = cur;
        a->anim.toRect = a->restoreRect;
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 255;

        a->maximized = false;                // 状态先行
        StartAnim(a, ANIM_RESTORE, animcfg::kRestoreMs);
    }

    void StartMinimize(AeroWnd* a)
    {
        if (a->anim.active || a->minimized) return;

        GetWindowRect(a->hwnd, &a->preMinimizeRect);

        a->anim.useRect = false;
        a->anim.useScale = true;
        a->anim.fromScale = 1.0f;
        a->anim.toScale = animcfg::kMinScale;
        a->anim.fromAlpha = 255;
        a->anim.toAlpha = 0;
        a->anim.anchorX = 0.5f;
        a->anim.anchorY = 0.92f;

        a->minimized = true;                 // 状态先行
        StartAnim(a, ANIM_MINIMIZE, animcfg::kMinimizeMs);
    }

    void StartTaskbarRestore(AeroWnd* a)
    {
        CancelAnim(a);

        a->anim.useRect = false;
        a->anim.useScale = true;
        a->anim.fromScale = animcfg::kTaskbarScale;
        a->anim.toScale = 1.0f;
        a->anim.fromAlpha = 0;
        a->anim.toAlpha = 255;
        a->anim.anchorX = 0.5f;
        a->anim.anchorY = 0.92f;

        a->minimized = false;
        StartAnim(a, ANIM_TASKBAR_RESTORE, animcfg::kTaskbarMs);
    }

    void ToggleMaximize(AeroWnd* a)
    {
        if (a->anim.active) return;
        if (a->minimized)   return;
        if (a->maximized) StartRestore(a);
        else              StartMaximize(a);
    }

    void OnAnimTick(AeroWnd* a)
    {
        const DWORD elapsed = GetTickCount() - a->anim.startMs;
        float t = (float)elapsed / (float)a->anim.durMs;
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) t = 0.0f;
        const float tRaw = t;

        // 分帧：把 t 量化成 k/steps，整段动画只落在 steps 个离散位置上
        // （窗口一格一格地跳，不是平滑滑动）。tRaw 到 1 时 k==steps，正好落在终点。
        if (a->anim.steps > 0)
        {
            int k = (int)(tRaw * (float)a->anim.steps);
            if (k > a->anim.steps) k = a->anim.steps;
            t = (float)k / (float)a->anim.steps;
        }

        // 缓动曲线：
        //   关闭            -> 缓入（t^2）
        //   打开 / 任务栏恢复 -> 回弹缓出（冲过头一点点再收回，这就是「弹出」的手感）
        //   换位置           -> smoothstep 缓入缓出（起停都不生硬）
        //   换位置 + overshoot -> back-out（过冲约 12%），配合分帧就是「甩到位再弹回来」
        //   其余            -> 缓出（1-(1-t)^3）
        float e;
        if (a->anim.type == ANIM_CLOSE)
        {
            e = t * t;
        }
        else if (a->anim.type == ANIM_OPEN || a->anim.type == ANIM_TASKBAR_RESTORE)
        {
            const float u = t - 1.0f;
            e = 1.0f + u * u * (2.9f * u + 1.9f);      // back-out，约 12% 过冲
        }
        else if (a->anim.type == ANIM_MOVE)
        {
            if (a->anim.overshoot)
            {
                const float u = t - 1.0f;
                e = 1.0f + u * u * (2.9f * u + 1.9f);  // back-out
            }
            else
            {
                e = t * t * (3.0f - 2.0f * t);         // smoothstep
            }
        }
        else
        {
            const float u = 1.0f - t;
            e = 1.0f - u * u * u;
        }

        // ---- 原地抖动：真实窗口位移 ----
        // 必须放在 useRect 分支之前，并且 useRect 保持 false——
        // 否则后面那条分支会用 fromRect 把位置覆盖回去。
        if (a->anim.type == ANIM_JOLT)
        {
            const float env = (1.0f - tRaw) * (1.0f - tRaw);   // 振幅衰减，t=1 时精确为 0
            const float ph = tRaw * 15.0f;                    // 约 2.4 个周期
            const LONG  ox = (LONG)(a->anim.joltAmp * env * sinf(ph));
            const LONG  oy = (LONG)(a->anim.joltAmp * 0.6f * env * sinf(ph * 0.78f + 1.1f));

            SetWindowPos(a->hwnd, nullptr,
                a->anim.fromRect.left + ox,
                a->anim.fromRect.top + oy,
                0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // ---- 矩形插值（最大化 / 还原 / 换位置）----
        if (a->anim.useRect)
        {
            RECT r;
            r.left = (LONG)(a->anim.fromRect.left + (a->anim.toRect.left - a->anim.fromRect.left) * e);
            r.top = (LONG)(a->anim.fromRect.top + (a->anim.toRect.top - a->anim.fromRect.top) * e);
            r.right = (LONG)(a->anim.fromRect.right + (a->anim.toRect.right - a->anim.fromRect.right) * e);
            r.bottom = (LONG)(a->anim.fromRect.bottom + (a->anim.toRect.bottom - a->anim.fromRect.bottom) * e);

            // 换位置时叠一条弧：垂直于位移方向偏移 sin(πe)。
            // 两端为 0、中间最大，所以起止点和目标位置都精确落位，
            // 中间走的是弧而不是直线。
            if (a->anim.type == ANIM_MOVE && a->anim.arcAmp != 0.0f)
            {
                const float dx = (float)(a->anim.toRect.left - a->anim.fromRect.left);
                const float dy = (float)(a->anim.toRect.top - a->anim.fromRect.top);
                const float len = sqrtf(dx * dx + dy * dy);
                if (len > 1.0f)
                {
                    const float nx = -dy / len;        // 位移方向逆时针 90°
                    const float ny = dx / len;
                    const float s = sinf(3.14159265f * e) * a->anim.arcAmp;
                    const LONG  ox = (LONG)(nx * s);
                    const LONG  oy = (LONG)(ny * s);
                    r.left += ox; r.right += ox;
                    r.top += oy; r.bottom += oy;
                }
            }

            // 弧线可能把窗口甩出工作区，这里夹回来。
            // （贴边时弧线被压平，看起来像沿着屏幕边缘滑过去。）
            if (a->anim.useBounds)
            {
                const LONG bw = r.right - r.left, bh = r.bottom - r.top;
                const RECT& bd = a->anim.bounds;
                if (r.left < bd.left) { r.left = bd.left;   r.right = bd.left + bw; }
                if (r.top < bd.top) { r.top = bd.top;    r.bottom = bd.top + bh; }
                if (r.right > bd.right) { r.right = bd.right;  r.left = bd.right - bw; }
                if (r.bottom > bd.bottom) { r.bottom = bd.bottom; r.top = bd.bottom - bh; }
            }

            SetWindowPos(a->hwnd, nullptr, r.left, r.top,
                r.right - r.left, r.bottom - r.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            // 尺寸变了要重建后备缓冲
            EnsureBuffer(a, r.right - r.left, r.bottom - r.top);
        }

        float scale = 1.0f;
        if (a->anim.useScale)
            scale = a->anim.fromScale + (a->anim.toScale - a->anim.fromScale) * e;

        // alpha 必须夹到 [0,255] 再转 BYTE。
        //
        // back-out / 回弹这类缓动的 e 会过冲到约 1.12，
        // 直接 (BYTE) 强转会**模 256 溢出**：
        //   打开动画 110 + (255-110)*1.1208 = 272 → (BYTE)272 = 16
        // 于是 alpha 走 110 → 255 → 16 → 255，
        // 画面表现就是"弹到最大时突然变近乎全透明，下一帧又弹回来"——
        // 也就是动画末尾闪的那一下。
        float af = (float)a->anim.fromAlpha +
            ((float)a->anim.toAlpha - (float)a->anim.fromAlpha) * e;
        if (af < 0.0f)   af = 0.0f;
        if (af > 255.0f) af = 255.0f;
        const BYTE alpha = (BYTE)af;

        PaintWindow(a, scale, alpha, a->anim.anchorX, a->anim.anchorY); 
        // 注意用 tRaw 判断结束：分帧后 t 在最后一格也会等于 1，但用原始进度更直观。
        if (tRaw >= 1.0f)
        {
            const AnimType finished = a->anim.type;
            a->anim.active = false;
            a->anim.type = ANIM_NONE;
            a->anim.useRect = false;
            a->anim.useScale = false;
            a->anim.arcAmp = 0.0f;
            a->anim.joltAmp = 0.0f;
            a->anim.useBounds = false;
            SetTickRate(a, a->opt.tickMs);

            if (finished == ANIM_CLOSE)
            {
                DestroyWindow(a->hwnd);
                // WM_NCDESTROY 已经把 a 释放了，后面不能再碰
                return;
            }
            else if (finished == ANIM_MINIMIZE)
            {
                ShowWindow(a->hwnd, SW_MINIMIZE);
            }
            else
            {
                a->hot = BTN_NONE;
                PaintWindow(a);
            }

            // 排队中的抖动：等这段动画彻底收完再抖
            if (a->joltQueued)
            {
                const float amp = a->joltQueuedAmp;
                const DWORD ms = a->joltQueuedMs;
                a->joltQueued = false;
                StartJolt(a, amp, ms);
            }
        }
    }

    // ------------------------------------------------------------ 系统菜单 ----
    void ShowSystemMenu(AeroWnd* a, POINT ptScreen)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        UINT flagsRestore = MF_STRING;
        UINT flagsMaximize = MF_STRING;
        if (a->maximized) flagsRestore |= MF_GRAYED;
        else              flagsMaximize |= MF_GRAYED;

        AppendMenuW(menu, flagsRestore, SC_RESTORE, L"还原(&R)");
        AppendMenuW(menu, MF_STRING, SC_MINIMIZE, L"最小化(&N)");
        AppendMenuW(menu, flagsMaximize, SC_MAXIMIZE, L"最大化(&X)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, SC_CLOSE, L"关闭(&C)");

        SetMenuDefaultItem(menu, SC_CLOSE, FALSE);

        const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
            ptScreen.x, ptScreen.y, 0, a->hwnd, nullptr);
        DestroyMenu(menu);

        if (cmd != 0) PostMessageW(a->hwnd, WM_SYSCOMMAND, (WPARAM)cmd, 0);
    }

    void CenterOnScreen(int w, int h, int& x, int& y)
    {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    }

    // ------------------------------------------------------------ 窗口过程 ----
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        AeroWnd* a = From(hwnd);

        switch (msg)
        {
        case WM_NCCREATE:
        {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return TRUE;
        }

        // ------------------------------------------------------- 命中测试 ----
        case WM_NCHITTEST:
        {
            if (!a) break;

            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT wr; GetWindowRect(hwnd, &wr);

            const int x = pt.x - wr.left;
            const int y = pt.y - wr.top;
            const int W = wr.right - wr.left;
            const int H = wr.bottom - wr.top;

            const int bodyL = kShadow;
            const int bodyR = W - kShadow;
            const int bodyT = kShadow;
            const int bodyB = H - kShadow;

            const POINT cp = { x, y };
            const bool onButton = a->opt.buttons &&
                (PtInRect(&a->rcClose, cp) || PtInRect(&a->rcMax, cp) ||
                    PtInRect(&a->rcMin, cp));
            const bool onTitle = (y >= bodyT && y < bodyT + kTitleH);

            // 边缘拖拽缩放（最大化时不响应）
            if (a->opt.resizable && !a->maximized)
            {
                const int RS = kResizeHot;
                const bool nearL = (x >= bodyL - RS && x < bodyL);
                const bool nearR = (x >= bodyR && x < bodyR + RS);
                const bool nearT = (y >= bodyT - RS && y < bodyT);
                const bool nearB = (y >= bodyB && y < bodyB + RS);

                if (nearL && nearT) return HTTOPLEFT;
                if (nearR && nearT) return HTTOPRIGHT;
                if (nearL && nearB) return HTBOTTOMLEFT;
                if (nearR && nearB) return HTBOTTOMRIGHT;

                if (nearL) return HTLEFT;
                if (nearR) return HTRIGHT;
                if (nearT) return HTTOP;
                if (nearB) return HTBOTTOM;
            }

            if (x < kShadow || x >= W - kShadow ||
                y < kShadow || y >= H - kShadow)
                return HTTRANSPARENT;

            if (onButton) return HTCLIENT;
            if (onTitle)  return HTCAPTION;
            return HTCLIENT;
        }

        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = kMinContentW + kShadow * 2;
            mmi->ptMinTrackSize.y = kMinContentH + kShadow * 2;
            return 0;
        }

        // ------------------------------------------------------- 鼠标 ----
        case WM_MOUSEMOVE:
        {
            if (!a) break;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int nh = BTN_NONE;
            if (a->opt.buttons)
            {
                if (PtInRect(&a->rcClose, pt)) nh = BTN_CLOSE;
                else if (PtInRect(&a->rcMax, pt)) nh = BTN_MAX;
                else if (PtInRect(&a->rcMin, pt)) nh = BTN_MIN;
            }
            if (nh != a->hot) { a->hot = nh; PaintWindow(a); }

            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            if (a && a->hot != BTN_NONE) { a->hot = BTN_NONE; PaintWindow(a); }
            return 0;

        case WM_LBUTTONDOWN:
        {
            if (!a || !a->opt.buttons) return 0;
            const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

            if (PtInRect(&a->rcClose, pt))
            {
                elog::Write(L"[aero] 关闭按钮被点击: '%s'", a->opt.title.c_str());
                // 先通知上层「这是玩家主动关的」，再发 WM_CLOSE。
                // 顺序不能反：WM_CLOSE 会一路走到 StartClose，中间可能
                // 触发别的处理，先把标志立起来才最可靠。
                if (a->opt.onUserClose)
                    a->opt.onUserClose(hwnd, a->opt.onUserCloseUser);
                SendMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            else if (PtInRect(&a->rcMax, pt))
            {
                ToggleMaximize(a);
            }
            else if (PtInRect(&a->rcMin, pt))
            {
                StartMinimize(a);
            }
            return 0;
        }

        case WM_NCLBUTTONDBLCLK:
            if (wp == HTCAPTION && a) { ToggleMaximize(a); return 0; }
            break;

        case WM_NCRBUTTONUP:
            if (wp == HTCAPTION && a)
            {
                POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                ShowSystemMenu(a, pt);
                return 0;
            }
            break;

            // ------------------------------------------------------- 键盘 ----
        case WM_SYSKEYDOWN:
            if (wp == VK_SPACE && a)
            {
                RECT wr; GetWindowRect(hwnd, &wr);
                POINT pt = { wr.left + kShadow + 8, wr.top + kShadow + kTitleH };
                ShowSystemMenu(a, pt);
                return 0;
            }
            break;

        case WM_KEYDOWN:
            if (wp == VK_F11 && a) { ToggleMaximize(a); return 0; }
            break;

            // ------------------------------------------------------- 焦点 ----
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
            if (a)
            {
                const bool nowActive = (msg == WM_ACTIVATEAPP)
                    ? (wp != FALSE) : (LOWORD(wp) != WA_INACTIVE);
                if (nowActive != a->active)
                {
                    a->active = nowActive;
                    if (!a->anim.active) PaintWindow(a);
                }
            }
            return 0;

            // ------------------------------------------------------- 系统命令 ----
        case WM_SYSCOMMAND:
        {
            if (!a) break;
            const UINT cmd = (UINT)(wp & 0xFFF0);

            if (cmd == SC_CLOSE)
            {
                if (a->anim.type == ANIM_CLOSE) return 0;
                CancelAnim(a);

                // 从系统菜单 / Alt+F4 走来的「关闭」也算玩家主动关。
                // 注意：aero::AnimateClose() 走的是 WM_CLOSE，**不经过这里**，
                // 所以「到寿命自动淡出」不会被误判。
                if (a->opt.onUserClose)
                    a->opt.onUserClose(hwnd, a->opt.onUserCloseUser);

                if (a->minimized) { DestroyWindow(hwnd); return 0; }
                StartClose(a);
                return 0;
            }
            if (cmd == SC_MAXIMIZE) { StartMaximize(a); return 0; }
            if (cmd == SC_MINIMIZE) { StartMinimize(a); return 0; }

            if (cmd == SC_RESTORE)
            {
                elog::Write(L"[aero] SC_RESTORE: anim=%d active=%d max=%d min=%d",
                    (int)a->anim.type, (int)a->anim.active,
                    (int)a->maximized, (int)a->minimized);

                // 最小化动画进行中 -> 取消，回到正常
                if (a->anim.type == ANIM_MINIMIZE)
                {
                    CancelAnim(a);
                    a->minimized = false;
                    PaintWindow(a);
                    return 0;
                }
                // 已经最小化 -> 播放从任务栏恢复的动画
                if (a->minimized && !a->anim.active)
                {
                    a->minimized = false;
                    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    SetWindowPos(hwnd, nullptr,
                        a->preMinimizeRect.left, a->preMinimizeRect.top,
                        a->preMinimizeRect.right - a->preMinimizeRect.left,
                        a->preMinimizeRect.bottom - a->preMinimizeRect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
                    EnsureBuffer(a, a->preMinimizeRect.right - a->preMinimizeRect.left,
                        a->preMinimizeRect.bottom - a->preMinimizeRect.top);

                    PaintWindow(a, animcfg::kTaskbarScale, 0, 0.5f, 0.92f);
                    StartTaskbarRestore(a);
                    return 0;
                }
                // 最大化动画进行中 -> 取消并跳到还原位置
                if (a->anim.type == ANIM_MAXIMIZE)
                {
                    CancelAnim(a);
                    SetWindowPos(hwnd, nullptr,
                        a->restoreRect.left, a->restoreRect.top,
                        a->restoreRect.right - a->restoreRect.left,
                        a->restoreRect.bottom - a->restoreRect.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                    EnsureBuffer(a, a->restoreRect.right - a->restoreRect.left,
                        a->restoreRect.bottom - a->restoreRect.top);
                    a->maximized = false;
                    PaintWindow(a);
                    return 0;
                }
                // 已最大化 -> 走还原动画
                if (a->maximized && !a->anim.active) { StartRestore(a); return 0; }
                return 0;
            }
            break;
        }

        // ------------------------------------------------------- 定时器 ----
        case WM_TIMER:
            if (wp != kTickId || !a) break;

            ++a->frame;

            if (a->anim.active) { OnAnimTick(a); return 0; }

            // 静止时只重绘内容（尺寸没变）
            PaintWindow(a);
            return 0;

        case WM_SIZE:
            if (a && wp != SIZE_MINIMIZED)
            {
                // 动画期间直接返回：那几帧的位置/尺寸本来就在动，
                // OnAnimTick 下一帧会自己补上。在这里插一脚只会得到
                // 一帧全透明 —— EnsureBuffer 若因尺寸变化重建了 DIB，
                // 重建后的 bits 还没画过，直接呈现就是一片空白。
                if (a->anim.active) return 0;

                RECT wr; GetWindowRect(hwnd, &wr);
                EnsureBuffer(a, wr.right - wr.left, wr.bottom - wr.top);
                PaintWindow(a);
            }
            return 0;

        case WM_CLOSE:
            if (a)
            {
                elog::Write(L"[aero] WM_CLOSE '%s'", a->opt.title.c_str());
                if (a->anim.type == ANIM_CLOSE) return 0;
                CancelAnim(a);
                if (a->minimized) { DestroyWindow(hwnd); return 0; }
                StartClose(a);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_NCDESTROY:
        {
            if (a)
            {
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);

                for (size_t i = 0; i < g_windows.size(); ++i)
                    if (g_windows[i] == a) { g_windows.erase(g_windows.begin() + i); break; }

                if (a->memDC && a->oldBmp) SelectObject(a->memDC, a->oldBmp);
                if (a->bmp)   DeleteObject(a->bmp);
                if (a->memDC) DeleteDC(a->memDC);
                delete a;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        default: break;
        }

        return DefWindowProcW(hwnd, msg, wp, lp);
    }

} // namespace

namespace aero {

    HWND Create(HINSTANCE hInst, const Options& opt, PaintFn paint, void* user)
    {
        static bool registered = false;
        if (!registered)
        {
            WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = WndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            wc.lpszClassName = kClassName;
            wc.hIcon = g_iconBig;      // 任务栏 / Alt-Tab
            wc.hIconSm = g_iconSmall ? g_iconSmall : g_iconBig;
            if (!RegisterClassExW(&wc)) return nullptr;
            registered = true;
        }

        AeroWnd* a = new AeroWnd();
        a->opt = opt;
        a->paint = paint;
        a->user = user;

        const int W = opt.width + kShadow * 2;
        const int H = opt.height + kShadow * 2;

        int x = opt.x, y = opt.y;
        if (x == CW_USEDEFAULT || y == CW_USEDEFAULT) CenterOnScreen(W, H, x, y);

        DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
        if (opt.topmost) ex |= WS_EX_TOPMOST;

        a->hwnd = CreateWindowExW(ex, kClassName, opt.title.c_str(), WS_POPUP,
            x, y, W, H,
            nullptr, nullptr, hInst, a);
        if (!a->hwnd) { delete a; return nullptr; }

        if (!EnsureBuffer(a, W, H))
        {
            DestroyWindow(a->hwnd);
            return nullptr;
        }

        // 先按打开动画的初始状态画一帧，再显示，避免出现瞬间全尺寸的闪烁
        // 先启动动画状态，再用动画起点画首帧，最后才显示窗口。
        //
        // 顺序很要紧：ShowWindow 可能触发 WM_SIZE，而 WM_SIZE 里会检查
        // anim.active —— 如果此刻动画还没开始，它就会按「最终尺寸 + 全不透明」
        // 重画一帧，于是先闪一下完整窗口、再缩回去重播打开动画。
        // 把 StartOpen 提到 ShowWindow 之前，anim.active 已是 true，
        // 那一帧误绘自然被跳过。
        if (opt.animate)
        {
            StartOpen(a);
            PaintWindow(a, animcfg::kOpenScale, animcfg::kOpenAlpha);
            ShowWindow(a->hwnd, SW_SHOWNOACTIVATE);
        }
        else
        {
            PaintWindow(a);
            ShowWindow(a->hwnd, SW_SHOWNOACTIVATE);
            SetTimer(a->hwnd, kTickId, a->opt.tickMs, nullptr);
        }
        g_windows.push_back(a);
        return a->hwnd;
    }

    void Destroy(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (!a) return;
        CancelAnim(a);          // 别让收尾时还在跑动画
        DestroyWindow(hwnd);
    }

    // ---------------------------------------------------------------- 外观资源 ----
    // 图标：直接用 exe 自己的图标资源。
    // 素材早就编进去了（Ransom_dev.rc 里那条 `1 ICON`），所以单文件分发
    // 什么都不缺，也不用 LoadImageW 去读磁盘上的 .ico。
    void SetAppIconFromSelf()
    {
        HMODULE self = GetModuleHandleW(nullptr);

        // 指定尺寸让系统从图标组里挑最合适的那一张：
        // 大图标给任务栏 / Alt+Tab，小图标给标题栏。
        HICON big = (HICON)LoadImageW(self, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), 0);
        HICON sml = (HICON)LoadImageW(self, MAKEINTRESOURCEW(kAppIconResId), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), 0);

        if (!big && !sml)
        {
            elog::Write(L"[aero] 内置图标载入失败（资源 %d），err=%lu",
                        kAppIconResId, GetLastError());
            return;
        }

        if (g_iconBig)   DestroyIcon(g_iconBig);
        if (g_iconSmall) DestroyIcon(g_iconSmall);
        g_iconBig = big;
        g_iconSmall = sml ? sml : big;

        delete g_iconBmp;
        g_iconBmp = nullptr;
        if (Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHICON(g_iconSmall))
        {
            if (bmp->GetLastStatus() == Gdiplus::Ok) g_iconBmp = bmp;
            else                                     delete bmp;
        }

        elog::Write(L"[aero] 图标取自 exe 内置资源 %d（大=%d 小=%d 标题栏位图=%d）",
            kAppIconResId, big ? 1 : 0, sml ? 1 : 0, g_iconBmp ? 1 : 0);
    }

    void SetAppIcon(const wchar_t* icoPath)
    {
        if (!icoPath || !*icoPath) return;

        // LR_LOADFROMFILE + 指定尺寸：大图标给任务栏，小图标给标题栏
        HICON big = (HICON)LoadImageW(nullptr, icoPath, IMAGE_ICON,
            GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON),
            LR_LOADFROMFILE);
        HICON sml = (HICON)LoadImageW(nullptr, icoPath, IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_LOADFROMFILE);
        if (!big && !sml)
        {
            elog::Write(L"[aero] 图标载入失败: %s（err=%lu）", icoPath, GetLastError());
            return;
        }

        if (g_iconBig)   DestroyIcon(g_iconBig);
        if (g_iconSmall) DestroyIcon(g_iconSmall);
        g_iconBig = big;
        g_iconSmall = sml ? sml : big;      // 只有一个也照用

        // 标题栏要画的版本：HICON -> GDI+ Bitmap，缓存起来
        delete g_iconBmp;
        g_iconBmp = nullptr;
        if (Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHICON(g_iconSmall))
        {
            if (bmp->GetLastStatus() == Gdiplus::Ok) g_iconBmp = bmp;
            else                                     delete bmp;
        }

        elog::Write(L"[aero] 图标已载入: %s（大=%d 小=%d 标题栏位图=%d）",
            icoPath, big ? 1 : 0, sml ? 1 : 0, g_iconBmp ? 1 : 0);
    }

    bool SetTitleFontFromMemory(const unsigned char* data, size_t size,
                                const wchar_t* family)
    {
        if (g_titleFamily) { delete g_titleFamily; g_titleFamily = nullptr; }
        if (!data || size == 0 || !family || !*family) return false;

        // GDI+ 的 PrivateFontCollection::AddMemoryFont 和 GDI 的
        // AddFontMemResourceEx 都**不拷贝**这块内存，必须在整个进程生命周期
        // 内保持有效，所以留一份自己的拷贝。
        g_fontBytes.assign(data, data + size);

        // GDI 层面注册（标题栏之外的 GDI 绘制也能用到）。
        DWORD installed = 0;
        AddFontMemResourceEx((void*)g_fontBytes.data(), (DWORD)g_fontBytes.size(),
                             nullptr, &installed);

        if (!g_pfc) g_pfc = new PrivateFontCollection();
        if (g_pfc->AddMemoryFont(g_fontBytes.data(), (INT)g_fontBytes.size()) != Ok)
        {
            elog::Write(L"[aero] 内嵌字体注册失败，继续用默认字体");
            return false;
        }

        const INT n = g_pfc->GetFamilyCount();
        std::vector<FontFamily> fams(n > 0 ? n : 1);
        INT found = 0;
        if (n > 0) g_pfc->GetFamilies(n, fams.data(), &found);

        for (INT i = 0; i < found; ++i)
        {
            WCHAR nm[LF_FACESIZE] = {};
            if (fams[i].GetFamilyName(nm) != Ok) continue;
            if (_wcsicmp(nm, family) != 0) continue;

            g_titleFamily = fams[i].Clone();
            break;
        }

        const bool ok = (g_titleFamily != nullptr);
        elog::Write(L"[aero] 标题栏字体（内嵌 %zu 字节）-> %s（GDI 注册 %u 项，GDI+ 家族 %d 个，命中=%s）",
            size, family, installed, found, ok ? L"是" : L"否，将回退雅黑");
        return ok;
    }

    bool SetTitleFont(const wchar_t* fontFile, const wchar_t* family)
    {
        if (g_titleFamily) { delete g_titleFamily; g_titleFamily = nullptr; }
        if (!fontFile || !*fontFile || !family || !*family) return false;

        // FR_PRIVATE：只对本进程可见，不往系统里装字体。
        // 这一句 GDI 层面就够了（标题栏以外的 GDI 绘制也能用），
        // 但 GDI+ 还得单独喂一遍，见下面。
        const int gdiCount = AddFontResourceExW(fontFile, FR_PRIVATE, 0);

        if (!g_pfc) g_pfc = new PrivateFontCollection();
        if (g_pfc->AddFontFile(fontFile) != Ok)
        {
            elog::Write(L"[aero] 字体载入失败: %s，继续用默认字体", fontFile);
            return false;
        }

        const INT n = g_pfc->GetFamilyCount();
        std::vector<FontFamily> fams(n > 0 ? n : 1);
        INT found = 0;
        if (n > 0) g_pfc->GetFamilies(n, fams.data(), &found);

        for (INT i = 0; i < found; ++i)
        {
            WCHAR nm[LF_FACESIZE] = {};
            if (fams[i].GetFamilyName(nm) != Ok) continue;
            if (_wcsicmp(nm, family) != 0) continue;

            // 用 Clone() 拿一个堆上的副本：fams 是局部 vector，出了函数就没了
            g_titleFamily = fams[i].Clone();
            break;
        }

        const bool ok = (g_titleFamily != nullptr);
        elog::Write(L"[aero] 标题栏字体 %s -> %s（GDI 注册 %d 项，GDI+ 家族 %d 个，命中=%s）",
            fontFile, family, gdiCount, found, ok ? L"是" : L"否，将回退雅黑");
        return ok;
    }

    Gdiplus::FontFamily* UiFontFamily() { return g_titleFamily; }

    void SetWobble(HWND hwnd, bool on)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (!a) return;

        a->wobble = on;
        // 每个窗口一个随机相位，否则所有子窗口会整齐划一地一起晃
        a->wobbleSeed = (float)(rand() % 628) / 100.0f;
    }

    void AnimateMoveTo(HWND hwnd, int x, int y, DWORD ms, int w, int h)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (!a) return;
        if (ms < 60) ms = 60;
        StartMove(a, x, y, w, h, ms);
    }

    // 分帧 + 过冲版本：整段动画只走 steps 个离散位置，缓动用 back-out（过冲后弹回）。
    // arc=false 关掉弧线走直线——过冲和弧线叠在一起会把落点甩得很难看。
    void AnimateMoveToStepped(HWND hwnd, int x, int y, DWORD ms, int w, int h,
        int steps, bool overshoot, bool arc)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (!a) return;
        if (ms < 60) ms = 60;
        StartMove(a, x, y, w, h, ms, steps, overshoot, arc);
    }

    // 原地短促抖动：真实窗口位移，振幅按 (1-t)^2 衰减。
    // 和其它动画互斥，撞上时会排队等前面收完再抖（见 StartJolt）。
    void AnimateJolt(HWND hwnd, float amplitudePx, DWORD ms)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (a) StartJolt(a, amplitudePx, ms);
    }

    void AnimateClose(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (!a) return;
        if (!a->opt.animate) { DestroyWindow(hwnd); return; }
        SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }

    void AnimateMaximize(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (a) StartMaximize(a);
    }

    void AnimateRestore(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (a) StartRestore(a);
    }

    void AnimateMinimize(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (a) StartMinimize(a);
    }

    bool IsAlive(HWND hwnd)
    {
        return hwnd && IsWindow(hwnd) && From(hwnd) != nullptr;
    }

    int ShadowSize() { return kShadow; }

    // 想让 paint 回调拿到的内容区**正好**是 cw x ch，Options 里该填多少？
    //
    // 这一步很容易漏：Options::height 是**含标题栏**的（内容区 = height - kTitleH - 2），
    // 而 width/height 还各自要扣掉 1px 边框。直接把「内容尺寸」填进去，
    // 得到的画面会被纵向压扁约 12%（设计 340 高实际只画到 306）。
    int OptionsWidthForContent(int cw) { return cw + 2; }
    int OptionsHeightForContent(int ch) { return ch + kTitleH + 2; }

    RECT RectOf(HWND hwnd)
    {
        RECT r = { 0, 0, 0, 0 };
        if (hwnd && IsWindow(hwnd)) GetWindowRect(hwnd, &r);
        return r;
    }

    void Repaint(HWND hwnd)
    {
        AeroWnd* a = hwnd ? From(hwnd) : nullptr;
        if (a && !a->anim.active) PaintWindow(a);
    }

    int AliveCount() { return (int)g_windows.size(); }

} // namespace aero