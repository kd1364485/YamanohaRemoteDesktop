// ============================================================================
// pipeline_bench.cpp
// 本物の FramePipeline(キャプチャ → エンコード → 送信)を一定時間動かして、
// 段階ごとの所要時間・FPS・ビットレートを測る。あわせて、データチャネルの往復時間も測る。
//
// 使い方: pipeline_bench [秒数=15] [fps=60] [bitrate_kbps=4000] [ディスプレイ番号=プライマリ] [繰り返し=1]
//
// 構成: 送信側は本物の WebrtcStreamer(ホスト側と同じコード)。受信側は libdatachannel を
// 直接使った、同じプロセス内の素の受け手(実際のアプリでブラウザが担う役割)。
// FramePipeline のキャプチャは、WebRTC が接続されているときだけ動くので、この受け手が必要になる。
// 受信側は、映像を受け取って捨てる(デコードも表示もしない)。
//
// 測れるもの : ホスト内の処理時間(画面を取得してから、送信の呼び出しが返るまで)、
//              同一 PC 内でのデータチャネルの往復時間
// 測れないもの: 画面が変わってから、クライアントの画面に映るまで(ネットワーク、デコード、表示)。
//              これは別の端末が要る。
//
// 画面が静止していると、フレームがほとんど来ない(FPS が出ない)。
// 測るときは、画面に動きがある状態にする。
// YAMANOHA_STAGE_STATS を定義してビルドする(CMakeLists.txt のこのターゲットで設定済み)。
// ============================================================================
#include "capture/desktop_duplicator.h"
#include "capture/display_manager.h"
#include "gpu/d3d11_device.h"
#include "pipeline/frame_pipeline.h"
#include "webrtc/webrtc_streamer.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <timeapi.h>

using namespace yamanoha;
using Clock = std::chrono::steady_clock;

namespace {

const char* VendorName(GpuVendor v) {
    switch (v) {
        case GpuVendor::Intel: return "Intel";
        case GpuVendor::Nvidia: return "NVIDIA";
        case GpuVendor::Amd: return "AMD";
        default: return "Unknown";
    }
}

unsigned long long NowMicros() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count());
}

std::mutex g_rtt_mutex;
std::vector<double> g_rtt_us;

void PrintAndClearRtt() {
    std::lock_guard<std::mutex> lock(g_rtt_mutex);
    std::printf("\n--- データチャネルの往復時間(同一 PC 内のループバック、µs) ---\n");
    if (g_rtt_us.empty()) {
        std::printf("  応答なし(データチャネルが開いていない可能性)\n");
        return;
    }
    auto v = g_rtt_us;
    g_rtt_us.clear();
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) sum += x;
    auto pct = [&](double p) { return v[static_cast<size_t>(p * (v.size() - 1))]; };
    std::printf("  件数 %zu  平均 %.0f  p50 %.0f  p95 %.0f  p99 %.0f  最大 %.0f\n", v.size(),
                sum / v.size(), pct(0.50), pct(0.95), pct(0.99), v.back());
}

}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const int seconds = argc > 1 ? std::atoi(argv[1]) : 15;
    const int fps = argc > 2 ? std::atoi(argv[2]) : 60;
    const int bitrate_kbps = argc > 3 ? std::atoi(argv[3]) : 4000;
    const int display_arg = argc > 4 ? std::atoi(argv[4]) : -1;
    const int repeats = argc > 5 ? std::atoi(argv[5]) : 1;

    std::printf("========================================\n");
    std::printf("  FramePipeline ベンチ: %d 秒 x %d 回, %d fps, %d kbps\n", seconds, repeats, fps,
                bitrate_kbps);
    std::printf("========================================\n");

    // 実際のホストアプリ(Tauri)は DPI に対応して動く。条件を合わせる。
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 環境変数 YAMANOHA_BENCH_TIMER_1MS=1 のとき、タイマーの粒度を 1ms に上げる(既定は約 15.6ms)。
    // パイプラインは、フレーム間隔の残りを sleep_for で待つので、粒度が粗いと FPS が下がる。
    if (const char* t = std::getenv("YAMANOHA_BENCH_TIMER_1MS"); t && t[0] == '1') {
        timeBeginPeriod(1);
        std::printf("タイマーの粒度: 1 ms に設定\n");
    } else {
        std::printf("タイマーの粒度: 既定(約 15.6 ms)\n");
    }

    // 実行環境の確認: Desktop Duplication は、対話型のデスクトップ(通常は WinSta0 の Default)でしか動かない
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        char station[128] = "?", desktop[128] = "?";
        DWORD len = 0;
        GetUserObjectInformationA(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), &len);
        GetUserObjectInformationA(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop,
                                  sizeof(desktop), &len);
        std::printf("実行環境: セッション %lu, WindowStation=%s, Desktop=%s\n", session, station,
                    desktop);
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        std::printf("COM 初期化に失敗: 0x%08lX\n", hr);
        return 1;
    }

    // ディスプレイの一覧と、その接続先の GPU(ハイブリッド GPU かどうかが分かる)
    auto displays = DisplayManager::EnumerateDisplays();
    if (displays.empty()) {
        std::printf("ディスプレイが見つかりません\n");
        return 1;
    }
    int chosen = -1;
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& d = displays[i];
        std::printf("  [%zu] %ls  %ux%u  %.0f Hz  GPU: %ls (%s, adapter %d)%s\n", i, d.name.c_str(),
                    d.width, d.height, d.refresh_rate, d.adapter_name.c_str(),
                    VendorName(d.vendor), d.adapter_index, d.is_primary ? "  <- プライマリ" : "");
        if (display_arg < 0 && d.is_primary && chosen < 0) chosen = static_cast<int>(i);
    }
    if (display_arg >= 0 && display_arg < static_cast<int>(displays.size())) chosen = display_arg;
    if (chosen < 0) chosen = 0;
    const Display& display = displays[chosen];
    std::printf("  → ディスプレイ [%d] を測定します\n\n", chosen);

    // 事前確認: 本物の画面を取得できるか。ダミー画像(フォールバックモード)で測った数字は
    // 意味がないので、その場合は測定せずに止まる。
    {
        D3D11DeviceManager probe_device;
        if (!probe_device.Initialize(display.adapter_index)) {
            std::printf("D3D11 デバイスを作れませんでした\n");
            return 2;
        }
        DesktopDuplicator probe;
        const bool ok = probe.Initialize(probe_device.GetDevice(), probe_device.GetContext(),
                                         display.output_index);
        const bool dummy = !ok || probe.IsFallbackMode();
        probe.Shutdown();
        if (dummy) {
            // 環境変数 YAMANOHA_BENCH_ALLOW_DUMMY=1 のときだけ、ダミー画像のまま続ける。
            // その場合、キャプチャ取得の数字と FPS は、実際の画面のものではない。
            if (const char* allow = std::getenv("YAMANOHA_BENCH_ALLOW_DUMMY");
                allow && allow[0] == '1') {
                std::printf("\n*** 注意: Desktop Duplication が使えないため、ダミー画像で測定します。"
                            "キャプチャ取得の数字は無効です(GPU 間コピー・エンコード・送信は有効)。***\n\n");
            } else {
                std::printf("\n*** 画面の取得(Desktop Duplication)に失敗し、ダミー画像のモードになりました。"
                            "この状態の数字は、実際の画面を取り込んだものではないので、測定を中止します。"
                            "(ダミー画像で続けるには、環境変数 YAMANOHA_BENCH_ALLOW_DUMMY=1)***\n");
                return 2;
            }
        } else {
            std::printf("事前確認: 本物の画面を取得できます(Desktop Duplication)\n\n");
        }
    }

    const auto t_start = Clock::now();
    auto since_start = [&] {
        return std::chrono::duration<double>(Clock::now() - t_start).count();
    };

    // ---- 送信側: 本物の WebrtcStreamer ----
    auto sender = std::make_unique<WebrtcStreamer>();
    std::atomic<bool> sender_up{false}, receiver_up{false};
    sender->SetStateChangeCallback([&](bool c) { sender_up = c; });

    // データチャネルの往復: 受け手が "ping <時刻>" を送り、送信側がそのまま "pong <時刻>" で返す
    sender->SetMessageCallback([&](const std::string& msg) {
        if (msg.rfind("ping ", 0) == 0) {
            sender->SendMessageOverDataChannel("pong " + msg.substr(5));
        }
    });

    // ---- 受信側: 素の受け手(実際のアプリではブラウザが担う) ----
    rtc::Configuration receiver_config;  // STUN は使わない(同一 PC なので、ホスト候補だけで足りる)
    auto receiver = std::make_shared<rtc::PeerConnection>(receiver_config);
    std::mutex receiver_mutex;
    std::shared_ptr<rtc::DataChannel> receiver_channel;
    std::vector<std::pair<std::string, std::string>> pending_candidates;
    bool answer_delivered = false;

    receiver->onStateChange([&](rtc::PeerConnection::State s) {
        receiver_up = (s == rtc::PeerConnection::State::Connected);
    });
    // 映像は受け取って捨てる。受け手が受信用のトラックを持っていないと、
    // libdatachannel は Answer で映像を拒否する(m=video の port が 0 になる)ので、
    // ブラウザと同じように、送信側と同じ mid の受信専用トラックを用意しておく。
    rtc::Description::Video receive_video("video", rtc::Description::Direction::RecvOnly);
    receive_video.addH264Codec(96);
    auto receive_track = receiver->addTrack(receive_video);
    // 受け取ったパケットを読み出して捨てる。読まないと受信側のキューが溢れ、
    // libdatachannel が「dropped due to a full queue」を毎秒 200〜300 件出していた(2026/11/06 に確認)。
    receive_track->onMessage([](rtc::message_variant) {});
    receiver->onTrack([](std::shared_ptr<rtc::Track> t) {
        std::printf("[受信側] トラックを受け取った: mid=%s\n", t->mid().c_str());
    });
    receiver->onDataChannel([&](std::shared_ptr<rtc::DataChannel> dc) {
        dc->onMessage([](rtc::message_variant m) {
            if (!std::holds_alternative<std::string>(m)) return;
            const auto& msg = std::get<std::string>(m);
            if (msg.rfind("pong ", 0) != 0) return;
            const auto sent_us = std::strtoull(msg.c_str() + 5, nullptr, 10);
            std::lock_guard<std::mutex> lock(g_rtt_mutex);
            g_rtt_us.push_back(static_cast<double>(NowMicros() - sent_us));
        });
        std::lock_guard<std::mutex> lock(receiver_mutex);
        receiver_channel = dc;
    });
    // Answer を送信側へ渡す。それより前に出た ICE 候補は、Answer のあとにまとめて渡す
    receiver->onLocalDescription([&](rtc::Description d) {
        if (d.typeString() != "answer") return;
        if (std::getenv("YAMANOHA_BENCH_DEBUG_SDP")) {
            std::istringstream lines{std::string(d)};
            for (std::string line; std::getline(lines, line);) {
                if (line.rfind("m=", 0) == 0 || line.rfind("a=recvonly", 0) == 0 ||
                    line.rfind("a=sendonly", 0) == 0 || line.rfind("a=inactive", 0) == 0 ||
                    line.rfind("a=sendrecv", 0) == 0 || line.rfind("a=rtpmap", 0) == 0) {
                    std::printf("   [answer] %s\n", line.c_str());
                }
            }
        }
        sender->ProcessSignalingMessage("answer", std::string(d));
        std::lock_guard<std::mutex> lock(receiver_mutex);
        answer_delivered = true;
        for (auto& c : pending_candidates) {
            sender->ProcessSignalingMessage("candidate", c.first, c.second);
        }
        pending_candidates.clear();
    });
    receiver->onLocalCandidate([&](rtc::Candidate c) {
        std::lock_guard<std::mutex> lock(receiver_mutex);
        if (answer_delivered) {
            sender->ProcessSignalingMessage("candidate", c.candidate(), c.mid());
        } else {
            pending_candidates.emplace_back(c.candidate(), c.mid());
        }
    });

    // 実際のアプリと同じ形: ホスト(送信側)の Offer を、受け手に渡す。
    // WebrtcStreamer は、ICE 候補の収集が終わるまで Offer を出さない(後述の接続時間を参照)。
    sender->SetLocalDescriptionCallback([&](const std::string& sdp, const std::string& type) {
        std::printf("[%.1fs] 送信側が %s を出力(%zu 文字)\n", since_start(), type.c_str(), sdp.size());
        if (type == "offer") receiver->setRemoteDescription(rtc::Description(sdp, "offer"));
    });

    if (!sender->Initialize()) {
        std::printf("WebrtcStreamer の初期化に失敗しました\n");
        return 1;
    }
    for (int i = 0; i < 900 && !(sender_up && receiver_up); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!sender_up || !receiver_up) {
        std::printf("WebRTC の接続が 90 秒以内に確立しませんでした\n");
        return 1;
    }
    std::printf("[%.1fs] WebRTC 接続(ループバック)確立  ← 起動から接続までの時間\n", since_start());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // データチャネルが開くのを待つ

    for (int run = 1; run <= repeats; ++run) {
        std::printf("\n################ 測定 %d / %d ################\n", run, repeats);

        // 本物のパイプラインを開始する
        FramePipeline pipeline;
        if (!pipeline.Start(display, fps, bitrate_kbps, sender.get())) {
            std::printf("FramePipeline の開始に失敗しました\n");
            return 1;
        }
        sender->SetKeyFrameRequestCallback([&] { pipeline.RequestKeyFrame(); });
        sender->SetBitrateRecommendationCallback(
            [&](uint32_t bps) { pipeline.OnBitrateEstimate(bps); });
        pipeline.StartThreads();

        // 実行中、100ms ごとに ping を送る
        std::atomic<bool> pinging{true};
        std::thread pinger([&] {
            while (pinging) {
                std::shared_ptr<rtc::DataChannel> dc;
                {
                    std::lock_guard<std::mutex> lock(receiver_mutex);
                    dc = receiver_channel;
                }
                if (dc && dc->isOpen()) dc->send("ping " + std::to_string(NowMicros()));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        std::this_thread::sleep_for(std::chrono::seconds(seconds));

        pinging = false;
        pinger.join();
        pipeline.PrintStageStats();
        PrintAndClearRtt();

        sender->SetKeyFrameRequestCallback(nullptr);
        sender->SetBitrateRecommendationCallback(nullptr);
        pipeline.Stop();
    }

    sender->Shutdown();
    receiver->close();
    CoUninitialize();
    return 0;
}
