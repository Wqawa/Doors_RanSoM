// ============================================================================
//  face.h
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace face {

    enum Mode {
        FACE_HIDDEN = 0,
        FACE_IDLE,
        FACE_STOP,
        FACE_ATTACK,
        FACE_THANKS,
        FACE_LOADING,
    };

    bool Start(HINSTANCE hInst);
    void Stop();

    // 头在屏幕任意位置浮现。
    // 开头的 kIdleBlackMs（100ms）内是**纯黑剪影**，之后才露出原图。
    void SpawnAnywhere(DWORD lifeMs);

    // 头瞬移到屏幕正中（无停牌叠加）。
    void MoveToCenter(DWORD lifeMs);

    // 头瞬移到屏幕正中，**并叠加停牌**（停牌在头之上，居中于头）。
    //   headLifeMs  头显示多久（到点整个 face 隐藏）
    //   stopLifeMs  停牌显示多久（先到点：停牌消失，头继续；传 0 = 跟头一起消失）
    // 两个时长都是从本函数被调用的那一刻算起。
    void MoveToCenterWithStop(DWORD headLifeMs, DWORD stopLifeMs);

    // 头重新露出：撤掉停牌叠加，头继续显示。
    void ShowHeadAgain(DWORD lifeMs);

    // 停牌闪现（头隐藏，只有停牌）—— 旧接口，保留兼容，主流程不再用。
    void ShowStopSign(DWORD lifeMs);

    // 开场 jumpscare（张口脸 + 深红底）。抖动 + 白处闪红 + 尺寸变化。
    //
    // smallToBig = true （默认）：先按停牌大小显示 kAttackSmallMs（100ms），
    //                            再瞬间跳到全屏 —— 开场那个"停牌炸开成脸"。
    // smallToBig = false        ：直接全屏，跳过那 100ms 的小尺寸。
    //                            给惩罚跳杀用 —— 玩家已经等了 90 秒，
    //                            再来一遍"小→大"像是在重播开场。
    void ShowAttackStill(DWORD lifeMs, bool smallToBig = true);

    // 惩罚 jumpscare（张口脸，不铺底）。同样先小后大 + 抖动 + 白处闪红。
    void ShowAttackShaking(DWORD lifeMs);
    void ShowAttack(DWORD lifeMs);   // = ShowAttackShaking 别名

    // 加载画面：走条 fillMs + FINISH 停留 finishMs。
    void ShowLoading(DWORD fillMs, DWORD finishMs);

    // 致谢画面。
    void ShowThanks(DWORD lifeMs);

    void SpawnIdle(DWORD lifeMs);    // = SpawnAnywhere 别名

    void Hide();

    Mode Current();
    bool Visible();
    bool LoadingDone();
    RECT IdleRect();

    // 子窗口内容贴图（popup 用）
    bool UsingAssets();
    int  PopupImageCount();
    bool BlitPopupImage(HDC hdc, const RECT& rc, int index);
    void BlitFace(HDC hdc, const RECT& rc, bool gape, DWORD frame);
    void BlitCrucified(HDC hdc, const RECT& rc, DWORD frame);
    void BlitStopSign(HDC hdc, const RECT& rc, float angleDeg);

    // 开发用：把程序生成的素材导出成 PNG 后退出。
    bool DumpAssets(const wchar_t* dir);

} // namespace face