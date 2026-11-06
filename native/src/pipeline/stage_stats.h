// ============================================================================
// stage_stats.h
// パイプラインの段階ごとの所要時間を記録する、測定用の部品
//
// YAMANOHA_STAGE_STATS を定義したときだけ有効になる。定義しないビルド(通常のビルド)では
// マクロがすべて空になり、パイプラインのコードには何も残らない。
// 使い方は test/pipeline_bench.cpp を参照。
// ============================================================================
#pragma once

#ifdef YAMANOHA_STAGE_STATS

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace yamanoha {

enum class Stage {
    Acquire,        // Desktop Duplication から新しいフレームを取得
    Copy,           // キャプチャ GPU → CPU(ステージング)→ カーソル合成 → エンコード GPU
    Submit,         // エンコーダへの投入(同期モードなので、エンコード自体もここで終わる)
    EncodeWait,     // 完了フレームの取り出し
    QueueWait,      // エンコード済みフレームが、送信キューで待った時間
    Push,           // WebrtcStreamer::PushVideoFrame の呼び出し
    CaptureToSend,  // キャプチャ処理の開始から、送信の呼び出しが返るまで(ホスト内の合計)
    FrameBytes,     // エンコード後のフレームのサイズ(バイト)
    kCount
};

enum class Counter {
    BacklogSkip,    // 処理中のフレームが上限に達して、キャプチャを見送った回数
    AcquireMiss,    // フレームを取得できなかった回数
    PushFail,       // 送信の呼び出しが false を返した回数(失敗したフレームは、FPS に数えない)
    kCount
};

class StageStats {
public:
    using Clock = std::chrono::steady_clock;

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& v : samples_) v.clear();
        for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
        started_ = Clock::now();
    }

    void Add(Stage stage, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_[static_cast<size_t>(stage)].push_back(value);
    }

    // t0 からの経過時間(µs)を記録する
    void AddSince(Stage stage, Clock::time_point t0) {
        Add(stage, std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    }

    // steady_clock のエポックからの µs で記録された時刻から、今までの経過時間(µs)を記録する
    void AddSinceMicros(Stage stage, uint64_t since_epoch_us) {
        const auto now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count());
        Add(stage, static_cast<double>(now_us - since_epoch_us));
    }

    void Count(Counter c) {
        counters_[static_cast<size_t>(c)].fetch_add(1, std::memory_order_relaxed);
    }

    void Print() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const double seconds =
            std::chrono::duration<double>(Clock::now() - started_).count();
        const auto& sent = samples_[static_cast<size_t>(Stage::CaptureToSend)];
        const auto& bytes = samples_[static_cast<size_t>(Stage::FrameBytes)];
        double total_bytes = 0;
        for (double b : bytes) total_bytes += b;

        std::printf("\n--- 段階ごとの所要時間(µs)  計測時間 %.1f 秒 ---\n", seconds);
        std::printf("  %-40s %7s %9s %9s %9s %9s %9s\n", "段階", "件数", "平均", "p50", "p95",
                    "p99", "最大");
        PrintRow("1 キャプチャ取得 (AcquireFrame)", Stage::Acquire);
        PrintRow("2 GPU 間コピー (CopyToEncodeTexture)", Stage::Copy);
        PrintRow("3 エンコード (SubmitFrame, 同期)", Stage::Submit);
        PrintRow("4 完了フレームの取り出し", Stage::EncodeWait);
        PrintRow("5 送信キューでの待ち", Stage::QueueWait);
        PrintRow("6 送信呼び出し (PushVideoFrame)", Stage::Push);
        PrintRow("合計: キャプチャ開始 → 送信呼び出し完了", Stage::CaptureToSend);
        std::printf("\n  送信できたフレーム: %zu  → %.1f FPS\n", sent.size(),
                    seconds > 0 ? sent.size() / seconds : 0.0);
        std::printf("  エンコード後の合計: %.2f MB  → %.0f kbps(平均)\n", total_bytes / 1e6,
                    seconds > 0 ? total_bytes * 8 / seconds / 1000 : 0.0);
        if (!bytes.empty()) {
            auto sorted = bytes;
            std::sort(sorted.begin(), sorted.end());
            std::printf("  フレームサイズ: 平均 %.0f B / p50 %.0f B / 最大 %.0f B\n",
                        total_bytes / bytes.size(), sorted[sorted.size() / 2], sorted.back());
        }
        std::printf("  バックログで見送った回数: %llu / 取得できなかった回数: %llu / 送信に失敗した回数: %llu\n",
                    static_cast<unsigned long long>(
                        counters_[static_cast<size_t>(Counter::BacklogSkip)].load()),
                    static_cast<unsigned long long>(
                        counters_[static_cast<size_t>(Counter::AcquireMiss)].load()),
                    static_cast<unsigned long long>(
                        counters_[static_cast<size_t>(Counter::PushFail)].load()));
    }

private:
    void PrintRow(const char* name, Stage stage) const {
        auto v = samples_[static_cast<size_t>(stage)];
        if (v.empty()) {
            std::printf("  %-40s %7d\n", name, 0);
            return;
        }
        std::sort(v.begin(), v.end());
        double sum = 0;
        for (double x : v) sum += x;
        auto pct = [&](double p) { return v[static_cast<size_t>(p * (v.size() - 1))]; };
        std::printf("  %-40s %7zu %9.0f %9.0f %9.0f %9.0f %9.0f\n", name, v.size(),
                    sum / v.size(), pct(0.50), pct(0.95), pct(0.99), v.back());
    }

    mutable std::mutex mutex_;
    std::vector<double> samples_[static_cast<size_t>(Stage::kCount)];
    std::atomic<uint64_t> counters_[static_cast<size_t>(Counter::kCount)]{};
    Clock::time_point started_ = Clock::now();
};

} // namespace yamanoha

// 使う側のマクロ(有効時)
#define YAMANOHA_STAT_BEGIN(var) const auto var = ::yamanoha::StageStats::Clock::now()
#define YAMANOHA_STAT_END(stats, stage, var) (stats).AddSince(::yamanoha::Stage::stage, var)
#define YAMANOHA_STAT_COUNT(stats, counter) (stats).Count(::yamanoha::Counter::counter)

#else  // 無効時は、すべて空になる

#define YAMANOHA_STAT_BEGIN(var)
#define YAMANOHA_STAT_END(stats, stage, var)
#define YAMANOHA_STAT_COUNT(stats, counter)

#endif
