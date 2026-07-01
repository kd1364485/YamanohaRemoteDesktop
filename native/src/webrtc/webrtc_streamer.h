#pragma once
#include <string>
#include <memory>
#include <functional>
#include <cstdint>

// PimplパターンによるWebRTCストリーマー実装
class WebrtcStreamer {
public:
    WebrtcStreamer();
    ~WebrtcStreamer();

    // ストリーマーの初期化
    bool Initialize();
    // ストリーマーの終了処理
    void Shutdown();

    // コールバック登録
    // ローカルSDP生成時（Offer/Answer）のコールバック
    void SetLocalDescriptionCallback(std::function<void(const std::string& sdp, const std::string& type)> cb);
    // ローカルICE候補生成時のコールバック
    void SetLocalCandidateCallback(std::function<void(const std::string& candidate, const std::string& mid)> cb);
    // Data Channelでのメッセージ受信用（マウス/キーボード入力制御用）のコールバック
    void SetMessageCallback(std::function<void(const std::string& msg)> cb);
    // 接続状態変更時のコールバック
    void SetStateChangeCallback(std::function<void(bool connected)> cb);

    // シグナリングメッセージの処理
    // リモートのSDP (Offer/Answer) または ICE候補 を設定する
    bool ProcessSignalingMessage(const std::string& msg_type, const std::string& sdp_or_candidate, const std::string& mid = "");

    // 映像フレーム (H.264 NALUデータ) をWebRTCトラックへ送信
    bool PushVideoFrame(const uint8_t* data, size_t size, uint64_t timestamp_us);

    // Data Channel経由でのメッセージ送信
    bool SendMessageOverDataChannel(const std::string& msg);

    // 現在接続されているか
    bool IsConnected() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
