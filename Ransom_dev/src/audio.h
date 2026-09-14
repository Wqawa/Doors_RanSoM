// ============================================================================
//  audio.h
//
//  声音：播放 E:\desktop\a90 里的原版音频素材。
//
//  素材用到解码器的原因：
//    Windows 自带 WAV/MP3 解码，但**不带 Vorbis(OGG) 解码器**，
//    而这套素材里 5 个是 OGG。所以内嵌了两个公有领域的单文件解码器
//    （stb_vorbis / minimp3），见 audio_clip.cpp。
//
//  素材映射（四个演出音效各响一次，按时间线排列）：
//    PlayGlitch  <- jumpscare2.mp3                  (停牌出现 / 检测开始)
//    PlayCaught  <- Ransom_start_(...).ogg          (抓到移动的那一刻)
//    PlayRiser   <- Ransom_encounter.wav            (倒计时剩 15 秒叠加)
//    PlayHit     <- Glitchyhitfaster.ogg            (没付清的跳杀)
//    PlaySuccess <- ransom_success.ogg              (付清赎金)
//    PlayError   <- Ransom_UI_-_Error.ogg           (UI 错误)
//    PlayCoin    <- Ransomgold_increase.ogg         (拾取金币)
//    SetTheme    <- Ransom_full_theme.mp3   (R4NS0M，已裁到 1:30)
//    SetGlitchBed<- GEN_GLITCH_LOOP.ogg     (故障循环)
//
//  线程模型：主线程只改增益 / 递增请求计数，合成线程负责取值。无锁。
// ============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace audio {

// audioDir：素材目录（传 nullptr 交给 assets::AudioDir 解析，默认取
// 仓库里的 assets\audio\）。
// 素材缺失时不会崩，只是对应音效静音，并在日志里写明。
bool Start();
void Stop();
bool Active();

// 素材加载情况（调试用）
int  LoadedClips();
int  TotalClips();

// 总音量 0-100
void SetMaster(int level);

// ---- 常驻层 ----
void SetTheme(bool on);            // R4NS0M 主题曲循环
void SetThemeLevel(int percent);   // 主题曲音量 0-100（100 = SetTheme(true) 的满音量）
void SetGlitchBed(int level);      // 0-100，故障循环底噪

// ---- 一次性音效 ----
void PlayGlitch();                 // 停牌出现（jumpscare2）
void PlayCaught();                 // 抓到移动的那一刻（Ransom_start）
void PlayHit();                    // 没付清的跳杀（Glitchyhitfaster）
void PlayRiser();                  // 倒计时剩 15 秒叠加（Ransom_encounter）
void PlayError();                  // UI 错误
void PlayCoin();                   // 收到金币
void PlaySuccess();                // 赎回成功（ransom_success.ogg）

// 常驻层归零 + 停掉所有声部。
void Silence();

// 把主题曲的播放位置往前（负值则往后）跳 seconds 秒。
//
// 用于让音乐和玩家加速过的倒计时保持同步：玩家每关一个勒索子窗口，
// director 会扣倒计时，同时调这个让音乐往前跳相应的时长。
//
// 跳变本身会造成波形不连续（咔哒一声），所以内部会先归零增益，
// 再用约 34ms 淡入盖住。**底噪层（glitch bed）不受影响**。
//
// 若跳变后超出曲长：
//   * 主题曲会停在末尾不再循环（这一轮的音乐就结束了）
//   * 下一次 SetTheme(true) 会把它复位
void SeekThemeBy(double seconds);

// 把若干秒的实际混音渲染成 WAV（开发时验证素材确实出声）。
// 需要先 Start()。渲染结束后会把状态复位。
bool DumpMix(const wchar_t* path, int seconds);

// 把**处理后的主题曲**单独导成 WAV 后退出用。
// 用来试听「裁到 1:20 + 保持音高慢放到 1:30」的结果对不对——
// 加工参数是主观的，最终得耳朵说了算。
// 不需要音频设备：素材加载完成即可导出。
bool DumpTheme(const wchar_t* path);

} // namespace audio
