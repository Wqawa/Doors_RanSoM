// ============================================================================
//  popup.h
//
//  勒索窗口群。按 A-90 / Ransom 原本的逻辑：
//
//   被抓的那一刻：主勒索窗口 + 一批子窗口同时出现
//   主窗口：透明 Aero 风格，**始终置顶**，**没有最大化/最小化/关闭按钮**
//   子窗口：同样风格，**有按钮**，可以被一个个关掉
//   过一段时间：子窗口全部自动关闭
//   之后在剩余倒计时里：每隔一段时间随机冒出 1-2 个子窗口，
//                        存活一段时间后自己消失
//
//  直到付清赎金或倒计时归零，主窗口才会消失。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace popup {

bool Start(HINSTANCE hInst);
void Stop();

// 被抓：开主勒索窗口，并一次放出 childBurst 个子窗口。
void BeginRansom(int childBurst);

// 付完钱：开始那套收尾演出——
// 只留 A90 的头 + 黑底 → 弧线飞向屏幕中心并缩放到付钱窗口大小 →
// 背景切到 payup_bg → Accepta90 / Thankyou_sign 从小到大出场（同时播 ransom_success）
// → 停留片刻 → 用**原本的关闭动画**收场。
// 付钱排版（payup.ini）不可用时会直接跳过，主窗口留给 EndRansom 照常收掉。
void BeginPayup();

// 结束（付清 / 超时 / 回潜伏）：关掉主窗口与所有子窗口。
void EndRansom();

// 主窗口每帧要显示的状态。
// totalMs = 倒计时总长。popup 用它算出「已过去多少比例」，
// 好让子窗口的密度随剩余时间逐渐收紧；传 0 表示不做密度变化。
void SetStatus(int gold, int goal, DWORD remainMs, DWORD totalMs);

bool MainAlive();
int  Children();
// 累计「玩家主动关掉的子窗口」个数，并清零。
// director 每帧调一次，用于把勒索倒计时往前推 —— 每关一个子窗口扣 10 秒。
// **寿命到了自动淡出的窗口不计入**。
int ConsumePlayerClosedCount();
int  TotalSpawned();

} // namespace popup
