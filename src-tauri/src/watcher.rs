use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::mpsc::channel;
use once_cell::sync::Lazy;
use notify::{RecursiveMode, Watcher, RecommendedWatcher, Config, EventKind};
use tauri::Emitter;

use crate::config::is_text_file;
use crate::magic_comment::extract_magic_comment;
use crate::project::{read_file_safely, update_project_state};

// ファイル監視状態
static WATCHER_STATE: Lazy<Mutex<Option<(RecommendedWatcher, std::thread::JoinHandle<()>, PathBuf)>>> = Lazy::new(|| {
    Mutex::new(None)
});

// デバウンス用の最終更新時刻（ミリ秒）
static LAST_UPDATE_TIME: Lazy<std::sync::atomic::AtomicU64> = Lazy::new(|| {
    std::sync::atomic::AtomicU64::new(0)
});

#[tauri::command]
pub(crate) fn start_watching(path: &str, app: tauri::AppHandle) -> Result<(), String> {
    let source_path = Path::new(path);

    if !source_path.exists() {
        return Err("指定されたパスが存在しません".to_string());
    }

    // チャネルを作成
    let (tx, rx) = channel();

    // Watcherを作成
    let mut watcher: RecommendedWatcher = Watcher::new(
        tx,
        Config::default(),
    ).map_err(|e| format!("Watcherの作成に失敗: {}", e))?;

    // ソースパスを監視
    // RecursiveMode::Recursiveは
    //ディレクトリ内のすべてのサブディレクトリを監視するよーっていう設定
    watcher.watch(source_path, RecursiveMode::Recursive)
        .map_err(|e| format!("監視の開始に失敗: {}", e))?;

    // パスとAppHandleをクローン（スレッドに移動するため
    let path_buf = source_path.to_path_buf();
    let path_buf_clone = path_buf.clone();
    let app_clone = app.clone();

    // バックグラウンドスレッドでイベントを待機
    let handle = std::thread::spawn(move ||
    {
        for event in rx {
            match event {
                Ok(event) =>
                {
                    // Modify(Name(To))イベントのみ対象にする（ファイル保存完了時）
                    //状態を更新すべきかどうか
                    let should_update = matches!(event.kind,
                        EventKind::Modify(notify::event::ModifyKind
                                 ::Name(notify::event::RenameMode::To)));

                    if should_update {
                        // マジックコメント検知（デバウンスとは独立して各保存で確認する）
                        for p in event.paths.iter() {
                            if is_text_file(p) {
                                if let Ok(c) = read_file_safely(p) {
                                    if extract_magic_comment(&c).is_some() {
                                        eprintln!("マジックコメント検知: {}", p.display());
                                        let _ = app_clone.emit(
                                            "magic-comment-detected",
                                            p.to_string_lossy().to_string(),
                                        );
                                    }
                                }
                            }
                        }

                        let now = std::time::SystemTime::now()
                            .duration_since(std::time::UNIX_EPOCH)
                            .unwrap()
                            .as_millis() as u64;

                        let last_update = LAST_UPDATE_TIME.load(std::sync::atomic::Ordering::Relaxed);

                        // 500ms以内のイベントはスキップ（デバウンス）
                        if now - last_update < 500 {
                            eprintln!("デバウンス: スキップ（{}ms経過）", now - last_update);
                            continue;
                        }

                        LAST_UPDATE_TIME.store(now, std::sync::atomic::Ordering::Relaxed);

                        eprintln!("ファイルイベント検知: {:?}", event.kind);
                        if let Err(e) = update_project_state(&path_buf) {
                            eprintln!("プロジェクト状態の更新に失敗: {}", e);
                        } else {
                            let _ = app_clone.emit("project-updated", ());
                            eprintln!("project-updatedイベントを送信しました");
                        }
                    }
                }
                Err(e) => {
                    eprintln!("監視エラー: {:?}", e);
                }
            }
        }
    });

    //Watcherとスレッドハンドルを保存
    *WATCHER_STATE.lock().unwrap() = Some((watcher, handle, path_buf_clone));

    // 初回のプロジェクト状態更新
    update_project_state(source_path)?;

    Ok(())
}

#[tauri::command]
pub(crate) fn stop_watching() -> Result<(), String> {
    //Watcherとスレッドハンドルをクリア
    let mut state = WATCHER_STATE.lock().unwrap();
    *state = None;
    Ok(())
}
