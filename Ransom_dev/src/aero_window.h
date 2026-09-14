// ============================================================================
//  aero_window.h
//
//  透明 Aero 玻璃风格的弹出窗口（从最初的 main.cpp 改造而来）。
//
//  每个窗口都是独立实例，状态挂在 GWLP_USERDATA 上，所以可以同时开很多个。
//  保留了原来的观感：分层窗口逐像素透明、圆角、四周阴影、标题栏高光、
//  底部内阴影与反光、圆角标题栏按钮。
//
//  用途：Ransom 的主勒索窗口与子窗口。
//  主窗口用 buttons=false（**没有最大化/最小化/关闭**），子窗口用 buttons=true。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

// GDI+ 前置依赖（WIN32_LEAN_AND_MEAN 不会带进来）
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>

namespace aero {

struct Options {
        std::wstring title;
        int  width = 420;
        int  height = 260;
        int  x = CW_USEDEFAULT;   // 屏幕坐标（含阴影外扩）
        int  y = CW_USEDEFAULT;
        bool buttons = true;            // 是否显示 最小化/最大化/关闭
        bool topmost = true;
        bool resizable = true;          // 边缘可拖拽缩放
        bool animate = true;            // 开/关/最大化/还原/最小化 动画
        UINT tickMs = 33;               // 静止时的内容重绘间隔。
        // 窗口开得多的时候要调大，否则消息循环被
        // 重绘压满，动画定时器会被饿住（实测延迟近 200ms）

// 玩家**主动**要求关闭窗口时（点右上角 X 按钮，或系统菜单 / Alt+F4
// 里的「关闭」）回调。用途：勒索子窗口要区分「玩家关的」和
// 「寿命到了自己淡出的」—— 前者要扣倒计时，后者不扣。
//
// 回调发生在真正开始淡出动画**之前**，此时窗口仍然有效。
// 从 popup 那边调 `aero::AnimateClose()` 触发的关闭**不会**走这个
// 回调（那条路走的是 WM_CLOSE，跟玩家点关闭按钮的路不同），
// 所以「到寿命自动淡出」不会被误判成玩家关闭。
//
// 传 nullptr 表示不关心。
        void (*onUserClose)(HWND hwnd, void* user) = nullptr;
        void* onUserCloseUser = nullptr;
};

// 内容绘制回调。
// content 是内容区矩形（窗口客户区坐标，已扣掉阴影与标题栏）。
// frame 每帧递增，用来驱动动画。
typedef void (*PaintFn)(Gdiplus::Graphics& g, const Gdiplus::RectF& content,
                        DWORD frame, void* user);

// 创建窗口。成功返回句柄，失败返回 nullptr。
// 窗口带打开动画（缩放 + 淡入）。
HWND Create(HINSTANCE hInst, const Options& opt, PaintFn paint, void* user);

// ---- 外观资源（启动时调一次即可，之后的窗口都会带上）----

// 用**exe 自己的图标资源**当窗口图标。
// 图标早就编在 exe 里了（Ransom_dev.rc 的 `1 ICON`），所以单文件分发
// 也不需要外部 .ico。大图标给任务栏，小图标给标题栏。
void SetAppIconFromSelf();

// 从磁盘上的 .ico 注册窗口图标。空串 / nullptr = 不设。
// 只在想临时换图标时用；正常路径是上面那个。
void SetAppIcon(const wchar_t* icoPath);

// 从**内存**注册字体（素材内嵌，磁盘上没有 .ttf 文件了）。
// 内存必须一直有效（GDI+ 的 PrivateFontCollection 不拷贝），
// 所以内部会把字节留一份。
bool SetTitleFontFromMemory(const unsigned char* data, size_t size,
                            const wchar_t* family);

// 从磁盘上的字体文件注册。空串 = 恢复默认。
bool SetTitleFont(const wchar_t* fontFile, const wchar_t* family);

// 已注册 UI 字体的 FontFamily，没注册成功返回 nullptr。
// 弹窗模块要复用它——**不能按名字构造 Font**：FR_PRIVATE 注册的字体
// GDI+ 按名字查不到，会静默回退（踩过这个坑）。
Gdiplus::FontFamily* UiFontFamily();

// ---- 原地抖动 ----
// 轻微抖动 + 旋转（绕窗口中心）。振幅刻意压在「角点位移 < 3px」以内，
// 靠窗口自带的 14px 透明阴影边距吃掉位移，所以**不动窗口矩形**，
// 不影响鼠标命中与拖拽。静态时生效，动画播放期间自动让位。
void SetWobble(HWND hwnd, bool on);

// ---- 平滑换位置 ----
// 沿一条弧线平滑移动到新的左上角位置（**含阴影的屏幕坐标**，
// 和 RectOf() 同一套）。两端精确落位，中间走弧。
// w / h 传 0 表示保持当前尺寸；传具体值则移动过程中**同时改大小**。
// 已经有动画在跑、或窗口已最大化/最小化时忽略本次调用。
void AnimateMoveTo(HWND hwnd, int x, int y, DWORD ms, int w = 0, int h = 0);

// 分帧 + 过冲版本（付钱窗口飞向屏幕正中用的就是这个）。
//   steps     > 0：整段动画只走 steps 个离散位置，窗口一格一格地跳
//   overshoot    ：缓动换成 back-out，冲过目标约 12% 再弹回来
//   arc          ：是否走弧线；过冲时建议 false，两者叠加会把落点甩歪
// 时长和 AnimateMoveTo 是同一套语义（ms 是**整段**时长，不是每帧间隔）。
void AnimateMoveToStepped(HWND hwnd, int x, int y, DWORD ms, int w, int h,
                          int steps, bool overshoot = true, bool arc = false);

// 原地短促抖动：整扇窗绕当前位置做一次快速衰减的位移振荡。
// 用来给「重击 / 出场」配一下撞击感——和 SetWobble 的绘制层微抖不同，
// 这里走的是**真实的窗口位移**（SetWindowPos），幅度可以到几 px，
// 抖完在最后一帧精确回到原位（偏移量按 (1-t)^2 收敛到 0）。
//
// 与其它动画互斥：如果此刻有动画在跑（比如换位置 / 缩放），
// 本次调用会**排队**，等那段动画干干净净收完再抖——不会中途掐掉它，
// 免得带过冲的 move 停在半路、抖完就偏几像素回不来。
void AnimateJolt(HWND hwnd, float amplitudePx, DWORD ms);

// 立刻销毁（收尾用，不做动画）。
void Destroy(HWND hwnd);

// 播放关闭动画后再自行销毁（子窗口到寿命、点关闭按钮时用）。
void AnimateClose(HWND hwnd);

// 播放最大化 / 还原 / 最小化动画。
void AnimateMaximize(HWND hwnd);
void AnimateRestore(HWND hwnd);
void AnimateMinimize(HWND hwnd);

bool IsAlive(HWND hwnd);

// 四周阴影的厚度（px）。窗口外形尺寸 = 内容尺寸 + 2*它。
// 需要按「内容尺寸」反算窗口外形尺寸时用得上（比如付钱演出要把窗口
// 缩放到某个排版的内容区大小）。
int ShadowSize();

// 想让 paint 回调拿到的内容区正好是 cw x ch，Options 里该填多少？
// （Options 的 height **含标题栏**，width/height 还要各自扣 1px 边框；
//   直接填内容尺寸会得到一张纵向被压扁的画面。）
int OptionsWidthForContent(int cw);
int OptionsHeightForContent(int ch);

// 窗口当前屏幕矩形（含阴影）。
RECT RectOf(HWND hwnd);

// 请求重绘（内容变化时调）。
void Repaint(HWND hwnd);

// 当前存活的窗口数（调试用）。
int AliveCount();

} // namespace aero
