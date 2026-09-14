// ============================================================================
//  audio.cpp
// ============================================================================
#include "audio.h"

#include "assets.h"
#include "audio_clip.h"
#include "entity_log.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

namespace {

    const double kSampleRate = 44100.0;
    const double kTwoPi = 6.283185307179586;

    // ------------------------------------------------------------ 素材 ----
    // 素材**全部内嵌在 exe 里**（见 assets.h），按文件名取字节。
    // 想临时换成磁盘上的素材：--audio-dir 指一个目录即可。
    //
    // 四个演出音效各司其职，在时间线上各响一次：
    //   jumpscare2        停牌出现（**开始检测**的那一刻）
    //   Ransom_start      **抓到移动**的那一刻
    //   Ransom_encounter  **倒计时剩 15 秒**时叠加（riser）
    //   Glitchyhitfaster  **没付清**的超时跳杀
    enum ClipId {
        C_SPAWN = 0,     // jumpscare2.mp3               停牌出现（检测开始）
        C_CAUGHT,        // Ransom_start_(...).ogg       抓到移动的那一刻
        C_HIT,           // Glitchyhitfaster.ogg         没付清的跳杀
        C_RISER,         // Ransom_encounter.wav         倒计时剩 15 秒叠加
        C_SUCCESS,       // ransom_success.ogg           付清赎金
        C_ERROR,         // Ransom_UI_-_Error_(...).ogg  UI 错误
        C_COIN,          // Ransomgold_increase_(...).ogg 金币
        C_THEME,         // Ransom_full_theme.mp3        主题曲（已裁到 1:30）
        C_GLITCHLOOP,    // GEN_GLITCH_LOOP_(...).ogg    故障底噪
        C_COUNT
    };

    const wchar_t* kFileNames[C_COUNT] = {
        L"jumpscare2.mp3",
        L"Ransom_start_(132683318015866).ogg",
        L"Glitchyhitfaster.ogg",
        L"Ransom_encounter.wav",
        L"ransom_success.ogg",
        L"Ransom_UI_-_Error_(80099403859001).ogg",
        L"Ransomgold_increase_(97004792231127).ogg",
        L"Ransom_full_theme.mp3",
        L"GEN_GLITCH_LOOP_(135402425939071).ogg",
    };

    // 每个一次性音效的**起始播放偏移**（秒）。
    // 从 0 开始播就是从头；只有 jumpscare2 需要跳过开头一段铺垫——
    // 停牌出现的瞬间直接切进它炸开的高潮点，冲击力才对。
    // 顺序必须和 ClipId 严格对应，加音效时别忘了同步这张表。
    //
    // C_THEME / C_GLITCHLOOP 是常驻循环层，不经过 SpawnOneShot，
    // 这里的 0 只是占位——它们的位置由 SetTheme / SetGlitchBed 自己控制。
    const double kOneShotStartSec[C_COUNT] = {
        0.4,   // C_SPAWN       jumpscare2      —— 跳过前 0.4 秒铺垫
        0.0,   // C_CAUGHT      Ransom_start
        0.0,   // C_HIT         Glitchyhitfaster
        0.0,   // C_RISER       Ransom_encounter
        0.0,   // C_SUCCESS     ransom_success
        0.0,   // C_ERROR       Ransom_UI_-_Error
        0.0,   // C_COIN        Ransomgold_increase
        0.0,   // C_THEME       Ransom_full_theme   （循环层，不走这里）
        0.0,   // C_GLITCHLOOP  GEN_GLITCH_LOOP     （循环层，不走这里）
    };

    audio_clip::Clip g_clips[C_COUNT];
    int              g_loaded = 0;

    // ---- 主题曲后期处理 ----
    // 原始 Ransom_full_theme.mp3 是 82.2 秒。按演出要求：
    //   1:20（80 秒）之后的**不要**（截断）；
    //   前 1:20 **保持音高**慢放到 1:30（90 秒）。
    // 90 秒正好等于勒索倒计时长度，所以每轮从 0 秒起播就自动和倒计时对齐。
    const double kThemeKeepSec = 80.0;
    const double kThemeOutSec = 90.0;

    // SetTheme(true) 时的满音量。最后 15 秒的渐隐就是在这条线上按比例往下压。
    const double kThemeFullGain = 0.55;

    // ------------------------------------------------------------ 声部 ----
    const int kMaxOneShots = 8;

        // 主题曲跳变后的淡入时长（采样帧）。约 34ms @ 44100Hz。
    // 纯粹为了盖住跳变点波形不连续产生的咔哒声——这个时长足够短，
    // 听感上不会被当成"音乐断了一下"。
    const int kThemeSeekFadeFrames = 1500;

    // 主题曲跳变请求的通道（主线程 -> 合成线程）。
    // 和 g_req / g_themeRestart 同一套无锁模式：主线程只递增计数，
    // 合成线程看到计数变了才去动 pos / gain。
    std::atomic<int>  g_themeSeekReq{ 0 };
    int               g_themeSeekSeen = 0;
    std::atomic<long> g_themeSeekDeltaFrames{ 0 };

    // 循环层：增益平滑过渡，避免开关时爆音
    struct LoopLayer {
        int    clipId = -1;
        size_t pos = 0;
        double gain = 0.0;
        double target = 0.0;

        // 播到末尾是否回绕。主题曲默认循环；被 SeekThemeBy 往前跳过
        // 一次之后翻成 false —— 因为主题曲本来就是 90 秒对应 90 秒倒计时，
        // 玩家把倒计时扣到 0 就意味着这一轮音乐也该结束了，
        // 再循环会盖住后面的惩罚音效。
        bool loop = true;

        // SeekThemeBy 之后的淡入：还剩多少采样帧。
        // >0 时由 Sample() 按线性插值把 gain 从 0 拉回 target，
        // Tick() 在这段时间里不插手（否则两边会打架）。
        int  fadeInLeft = 0;

        void Tick()
        {
            if (fadeInLeft > 0) return;   // 淡入由 Sample 管
            gain += (target - gain) * 0.00025;
            if (gain < 0.0001 && target == 0.0) gain = 0.0;
        }

        double Sample()
        {
            if (clipId < 0) return 0.0;

            // ---- 跳变淡入 ----
            // 必须在 gain 检查之前递减：gain 可能是 0，也得让计数走。
            if (fadeInLeft > 0)
            {
                --fadeInLeft;
                gain = target * (1.0 - (double)fadeInLeft / (double)kThemeSeekFadeFrames);
                if (gain < 0.0) gain = 0.0;
            }

            if (gain <= 0.0) return 0.0;
            const audio_clip::Clip& c = g_clips[clipId];
            if (!c.Ok()) return 0.0;

            // 走到末尾：这一层停播（loop=false 时）。
            if (pos >= c.Frames())
            {
                if (!loop) return 0.0;
                pos = 0;
            }

            const double v = (c.pcm[pos] / 32768.0) * gain;
            ++pos;
            if (loop && pos >= c.Frames()) pos = 0;
            return v;
        }
    };

    struct OneShot {
        int    clipId = -1;
        size_t pos = 0;
        double gain = 1.0;
        bool   active = false;
    };

    LoopLayer g_theme;
    LoopLayer g_bed;
    OneShot   g_shots[kMaxOneShots];

    // 主线程递增这些计数，合成线程看差值起声部
    std::atomic<int> g_req[C_COUNT];
    int              g_seen[C_COUNT];

    // 主题曲「从头开始」的请求计数。和 g_req 同一套无锁写法：
    // 主线程只递增计数，合成线程发现计数变了才去动 pos，
    // 避免主线程直接写 pos 和合成线程打架。
    std::atomic<int> g_themeRestart{ 0 };
    int              g_themeRestartSeen = 0;

    std::atomic<int>  g_master{ 70 };
    HWAVEOUT          g_hwo = nullptr;
    HANDLE            g_thread = nullptr;
    std::atomic<bool> g_stop{ false };

    unsigned int g_rng = 0x12345678u;
    inline double RndBi()
    {
        g_rng = g_rng * 1664525u + 1013904223u;
        return ((double)((g_rng >> 8) & 0xFFFFFF) / 8388607.5) - 1.0;
    }

    // ------------------------------------------------------------ 起声部 ----
    void SpawnOneShot(int clipId)
    {
        if (clipId < 0 || clipId >= C_COUNT) return;

        // 该音效的起始偏移（帧）。素材没加载或偏移超过长度时退回 0，
        // 免得出现「pos 一进来就越界、声部立刻结束」的静默。
        size_t startPos = (size_t)(kOneShotStartSec[clipId] * kSampleRate);
        const audio_clip::Clip& c = g_clips[clipId];
        if (!c.Ok() || startPos >= c.Frames()) startPos = 0;

        for (int i = 0; i < kMaxOneShots; ++i)
        {
            if (g_shots[i].active) continue;
            g_shots[i].clipId = clipId;
            g_shots[i].pos = startPos;
            g_shots[i].gain = 1.0;
            g_shots[i].active = true;
            return;
        }
        // 满了就抢占最早的（听起来就是被新音效盖掉）
        g_shots[0].clipId = clipId;
        g_shots[0].pos = startPos;
        g_shots[0].gain = 1.0;
        g_shots[0].active = true;
    }

    // ------------------------------------------------------------ 混音 ----
    void Fill(short* out, int n)
    {
        const double mGain = g_master.load() / 100.0;

        // ---- 处理主线程的触发请求 ----
        for (int t = 0; t < C_COUNT; ++t)
        {
            const int cur = g_req[t].load();
            while (g_seen[t] < cur) { SpawnOneShot(t); ++g_seen[t]; }
        }

        // ---- 主题曲重头播：每一轮勒索都从 0 秒起，和 90 秒倒计时对齐 ----
        {
            const int cur = g_themeRestart.load();
            if (g_themeRestartSeen != cur)
            {
                g_themeRestartSeen = cur;
                g_theme.pos = 0;
                g_theme.loop = true;      // 新的一轮，恢复循环
                g_theme.fadeInLeft = 0;   // 清掉可能残留的淡入计数
            }
        }

        // ---- 主题曲跳变：玩家关子窗口时倒计时被扣，音乐跟着往前跳 ----
        {
            const int cur = g_themeSeekReq.load();
            if (g_themeSeekSeen != cur)
            {
                g_themeSeekSeen = cur;

                const long delta = g_themeSeekDeltaFrames.load();
                const audio_clip::Clip& c = g_clips[g_theme.clipId];
                if (c.Ok())
                {
                    long long p = (long long)g_theme.pos + (long long)delta;
                    const long long len = (long long)c.Frames();

                    // 前跳超过曲长：停在末尾。这一轮的音乐到此为止 ——
                    // 倒计时都已经归零了，音乐也该结束。
                    // 往后跳（负 delta）不可能小于 0，因为 pos 是累加过的
                    // 实际位置，delta 只会是在它基础上加。
                    if (p >= len)
                    {
                        p = len;
                        g_theme.loop = false;
                    }
                    if (p < 0) p = 0;

                    g_theme.pos = (size_t)p;

                    // 归零增益，让 Sample() 在 kThemeSeekFadeFrames 帧内
                    // 把它拉回 target —— 盖住波形不连续造成的咔哒声。
                    g_theme.gain = 0.0;
                    g_theme.fadeInLeft = kThemeSeekFadeFrames;
                }
            }
        }

        for (int i = 0; i < n; ++i)
        {
            double s = 0.0;

            g_theme.Tick();
            g_bed.Tick();

            s += g_theme.Sample();
            s += g_bed.Sample();

            for (int v = 0; v < kMaxOneShots; ++v)
            {
                OneShot& o = g_shots[v];
                if (!o.active) continue;

                const audio_clip::Clip& c = g_clips[o.clipId];
                if (!c.Ok() || o.pos >= c.Frames()) { o.active = false; continue; }

                s += (c.pcm[o.pos] / 32768.0) * o.gain;
                ++o.pos;
            }

            // ---- 合成的 tada（素材缺失时的替代）----
            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;

            out[i] = (short)(s * mGain * 32000.0);
        }
    }

    DWORD WINAPI AudioThread(LPVOID)
    {
        const int kBufs = 4;
        const int kSamples = 4096;

        std::vector<short>   data((size_t)kBufs * kSamples, 0);
        std::vector<WAVEHDR> hdr(kBufs);

        for (int i = 0; i < kBufs; ++i)
        {
            ZeroMemory(&hdr[i], sizeof(WAVEHDR));
            hdr[i].lpData = (LPSTR)&data[(size_t)i * kSamples];
            hdr[i].dwBufferLength = kSamples * sizeof(short);

            if (waveOutPrepareHeader(g_hwo, &hdr[i], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            {
                elog::Write(L"[audio] waveOutPrepareHeader 失败 (buf %d)", i);
                return 0;
            }
            Fill(&data[(size_t)i * kSamples], kSamples);
            waveOutWrite(g_hwo, &hdr[i], sizeof(WAVEHDR));
        }

        while (!g_stop.load())
        {
            for (int i = 0; i < kBufs && !g_stop.load(); ++i)
            {
                while (!(hdr[i].dwFlags & WHDR_DONE))
                {
                    if (g_stop.load()) break;
                    Sleep(5);
                }
                if (g_stop.load()) break;

                Fill(&data[(size_t)i * kSamples], kSamples);
                hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_hwo, &hdr[i], sizeof(WAVEHDR));
            }
        }

        waveOutReset(g_hwo);
        for (int i = 0; i < kBufs; ++i)
            waveOutUnprepareHeader(g_hwo, &hdr[i], sizeof(WAVEHDR));

        return 0;
    }

    // 主题曲后期：截到 kThemeKeepSec，再用 WSOLA 保持音高拉到 kThemeOutSec。
    // 素材缺失或太短就原样保留——**不致命**，不该因为这一步失败就没了主题曲。
    void EditTheme()
    {
        audio_clip::Clip& c = g_clips[C_THEME];
        if (!c.Ok()) return;

        const size_t keep = (size_t)(kThemeKeepSec * kSampleRate);
        if (c.Frames() <= keep)
        {
            elog::Write(L"[audio] 主题曲 %.2f 秒，不长于 %.0f 秒，跳过裁剪与慢放",
                c.Seconds(), kThemeKeepSec);
            return;
        }

        const double factor = kThemeOutSec / kThemeKeepSec;
        const double before = c.Seconds();

        audio_clip::Clip out;
        const DWORD t0 = GetTickCount();
        if (!audio_clip::StretchPitchPreserving(c, keep, factor, out))
        {
            elog::Write(L"[audio] 主题曲慢放失败，退回未处理版本（%.2f 秒）", before);
            return;
        }
        const DWORD ms = GetTickCount() - t0;

        elog::Write(L"[audio] 主题曲已处理：原 %.2f 秒 -> 保留前 %.1f 秒 -> 慢放 x%.4f -> %.2f 秒（耗时 %lums）",
            before, kThemeKeepSec, factor, out.Seconds(), (unsigned long)ms);

        c = out;
    }

} // namespace

namespace audio {

    bool Start()
    {
        // ---- 载入素材 ----
        // 名字是**文件名**，字节从内嵌资源里取（--audio-dir 时改读盘）。
        g_loaded = 0;
        assets::Blob blob;
        for (int i = 0; i < C_COUNT; ++i)
        {
            if (assets::Get(assets::KIND_AUDIO, kFileNames[i], blob))
                if (audio_clip::Load(kFileNames[i], blob.Data(), blob.Size(), g_clips[i]))
                    ++g_loaded;
        }

        elog::Write(L"[audio] 素材来源 %s —— 成功载入 %d / %d 个",
                    assets::Where(assets::KIND_AUDIO, kFileNames[0]).c_str(),
                    g_loaded, C_COUNT);

        // 顺便把一次性音效的起始偏移记一笔，排查「怎么开场就进高潮」时用得上。
        for (int i = 0; i < C_COUNT; ++i)
            if (kOneShotStartSec[i] > 0.0)
                elog::Write(L"[audio]   起播偏移: %s -> %.2f 秒",
                    kFileNames[i], kOneShotStartSec[i]);

        // ---- 主题曲：裁到 1:20，再保持音高慢放到 1:30 ----
        EditTheme();

        if (g_hwo) return true;

        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM;
        wf.nChannels = 1;
        wf.nSamplesPerSec = (DWORD)kSampleRate;
        wf.wBitsPerSample = 16;
        wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
        wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

        const MMRESULT mr = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
        if (mr != MMSYSERR_NOERROR)
        {
            elog::Write(L"[audio] waveOutOpen 失败 mr=%u（没有音频设备？静默降级）", (unsigned)mr);
            g_hwo = nullptr;
            return false;
        }

        for (int i = 0; i < C_COUNT; ++i) { g_req[i] = 0; g_seen[i] = 0; }

        g_theme.clipId = C_THEME;
        g_theme.pos = 0;
        g_theme.gain = 0.0;
        g_theme.target = 0.0;

        g_bed.clipId = C_GLITCHLOOP;
        g_bed.pos = 0;
        g_bed.gain = 0.0;
        g_bed.target = 0.0;

        g_stop = false;
        g_thread = CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
        if (!g_thread)
        {
            elog::Write(L"[audio] 合成线程创建失败, err=%lu", GetLastError());
            waveOutClose(g_hwo);
            g_hwo = nullptr;
            return false;
        }

        elog::Write(L"[audio] 已启动 %dHz 16bit 单声道", (int)kSampleRate);
        return true;
    }

    void Stop()
    {
        if (!g_hwo) return;

        g_stop = true;
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }

        waveOutClose(g_hwo);
        g_hwo = nullptr;

        for (int i = 0; i < C_COUNT; ++i) g_clips[i] = audio_clip::Clip();
        g_loaded = 0;
        g_themeRestartSeen = g_themeRestart.load();

        elog::Write(L"[audio] 已停止");
    }

    bool Active() { return g_hwo != nullptr; }

    int LoadedClips() { return g_loaded; }
    int TotalClips() { return C_COUNT; }

    void SetMaster(int level)
    {
        if (level < 0)   level = 0;
        if (level > 100) level = 100;
        g_master = level;
    }

    void SetTheme(bool on)
    {
        g_theme.target = on ? kThemeFullGain : 0.0;

        // 每一轮开场都从 0 秒起：主题曲处理完正好是 90 秒，
        // 和勒索倒计时等长，从头播就能对齐。
        // 只递增计数，真正的 pos 归零在合成线程里做（无锁约定）。
        if (on) ++g_themeRestart;

        // 注意：没 Start 过（--no-audio）时素材本来就没加载，
        // 这时候报「素材缺失」是误导，所以分开说。
        const wchar_t* why = !g_hwo ? L"（音频未启动）"
            : (g_clips[C_THEME].Ok() ? L"" : L"（素材缺失）");
        elog::Write(L"[audio] 主题曲 %s%s（%.2f 秒，从头起播）",
            on ? L"开启" : L"关闭", why, g_clips[C_THEME].Seconds());
    }

    void SetThemeLevel(int percent)
    {
        if (percent < 0)   percent = 0;
        if (percent > 100) percent = 100;

        // 故意**不写日志**：director 每帧（16ms）都会调这个来做渐隐，
        // 写日志会把整个文件刷爆（覆盖层那边已经吃过一次这个亏，见 LogGate）。
        g_theme.target = (percent / 100.0) * kThemeFullGain;
    }

    void SetGlitchBed(int level)
    {
        if (level < 0)   level = 0;
        if (level > 100) level = 100;
        g_bed.target = (level / 100.0) * 0.42;
    }

    void PlayGlitch() { ++g_req[C_SPAWN];      elog::Write(L"[audio] → spawn (jumpscare2, 起播 0.4s)"); }
    void PlayCaught() { ++g_req[C_CAUGHT];     elog::Write(L"[audio] → 被抓 (Ransom_start)"); }
    void PlayHit() { ++g_req[C_HIT];        elog::Write(L"[audio] → 跳杀 (Glitchyhitfaster)"); }
    void PlayRiser() { ++g_req[C_RISER];      elog::Write(L"[audio] → riser (Ransom_encounter)"); }
    void PlayError() { ++g_req[C_ERROR];      elog::Write(L"[audio] → UI 错误"); }
    void PlayCoin() { ++g_req[C_COIN];       elog::Write(L"[audio] → 金币"); }
    void PlaySuccess() { ++g_req[C_SUCCESS];    elog::Write(L"[audio] → 赎回成功 (ransom_success)"); }

    void Silence()
    {
        g_theme.target = 0.0;
        g_bed.target = 0.0;
        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;
    }

    void SeekThemeBy(double seconds)
    {
        // 没启动音频（--no-audio / 无声卡）：什么都不做。
        // 这里**不**报"主题曲未载入"那类日志 —— 静默降级是设计的一部分。
        if (!g_hwo) return;

        const long frames = (long)(seconds * kSampleRate);
        if (frames == 0) return;

        g_themeSeekDeltaFrames.store(frames);
        ++g_themeSeekReq;

        elog::Write(L"[audio] 主题曲位置跳变 %+.2f 秒（约 %ld 帧）",
            seconds, frames);
    }

    // ---------------------------------------------------------------- 导出 ----
    namespace {

        bool WriteWav16(const wchar_t* path, const short* data, int count)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;

            const DWORD dataBytes = (DWORD)(count * sizeof(short));
            const DWORD rate = (DWORD)kSampleRate;
            const WORD  channels = 1, bits = 16;
            const WORD  align = (WORD)(channels * bits / 8);
            const DWORD byteRate = rate * align;

            auto w32 = [&](DWORD v) { fwrite(&v, 4, 1, f); };
            auto w16 = [&](WORD  v) { fwrite(&v, 2, 1, f); };
            auto tag = [&](const char* s) { fwrite(s, 1, 4, f); };

            tag("RIFF"); w32(36 + dataBytes); tag("WAVE");
            tag("fmt "); w32(16); w16(1); w16(channels);
            w32(rate); w32(byteRate); w16(align); w16(bits);
            tag("data"); w32(dataBytes);
            fwrite(data, 1, dataBytes, f);
            fclose(f);
            return true;
        }

    } // namespace

    bool DumpMix(const wchar_t* path, int seconds)
    {
        if (!path || !*path || seconds < 1) return false;

        const int n = (int)(kSampleRate * seconds);
        std::vector<short> buf((size_t)n, 0);

        // 为了在短时间内听到所有素材，这里按短间隔依次触发
        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;
        for (int i = 0; i < C_COUNT; ++i) { g_req[i] = 0; g_seen[i] = 0; }

        const int savedMaster = g_master.load();
        g_master = 100;
        g_theme.target = kThemeFullGain;
        g_bed.target = 0.20;

        // 名字用**宽字符串**：源码是 UTF-8（/utf-8），窄字面量里的中文是
        // UTF-8 字节，而 elog 的 %S 会按 ACP(GBK) 把它转宽——日志里就成了
        // 双重编码的乱码。直接给宽字符就没这一层转换。
        struct Cue { double at; int clip; const wchar_t* name; };
        const Cue cues[] = {
            { 0.30, C_SPAWN,  L"spawn / jumpscare2 (从 0.4s 起)" },
            { 1.20, C_ERROR,  L"UI error"                  },
            { 2.20, C_COIN,   L"gold increase"             },
            { 3.00, C_CAUGHT, L"caught / Ransom_start"     },
            { 4.00, C_HIT,    L"hit / Glitchyhitfaster"    },
            { 5.50, C_RISER,  L"riser / Ransom_encounter"  },
        };
        const int cueCount = (int)(sizeof(cues) / sizeof(cues[0]));

        int pos = 0, nextCue = 0;
        const int chunk = 1024;

        while (pos < n)
        {
            const double t = (double)pos / kSampleRate;
            while (nextCue < cueCount && cues[nextCue].at <= t)
            {
                ++g_req[cues[nextCue].clip];
                elog::Write(L"[audio] 导出时间表 %.2fs -> %s", cues[nextCue].at, cues[nextCue].name);
                ++nextCue;
            }

            const int len = (pos + chunk <= n) ? chunk : (n - pos);
            Fill(&buf[(size_t)pos], len);
            pos += len;
        }

        const bool ok = WriteWav16(path, buf.data(), n);

        Silence();
        g_master = savedMaster;
        for (int i = 0; i < kMaxOneShots; ++i) g_shots[i].active = false;

        elog::Write(L"[audio] 已导出 %d 秒混音到 %s（成功=%d）", seconds, path, (int)ok);
        return ok;
    }

    bool DumpTheme(const wchar_t* path)
    {
        const audio_clip::Clip& c = g_clips[C_THEME];
        if (!c.Ok())
        {
            elog::Write(L"[audio] 主题曲未载入，无法导出");
            return false;
        }

        // 导出的是**已经处理过**的主题曲（裁到 1:20 + 慢放到 1:30），
        // 不是磁盘上那个原始 mp3。加工参数是主观的，试听得靠这个文件。
        const bool ok = WriteWav16(path, c.pcm.data(), (int)c.pcm.size());
        elog::Write(L"[audio] 主题曲导出%s（%.2f 秒，%zu 帧）",
            ok ? L"成功" : L"失败", c.Seconds(), c.Frames());
        return ok;
    }

} // namespace audio