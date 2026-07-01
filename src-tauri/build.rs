use std::env;
use std::path::Path;

fn main() {
    // C++ DLL のインポートライブラリ (yamanoha_core.lib) へのサーチパスを設定
    println!("cargo:rustc-link-search=native=../native/build/Debug");
    println!("cargo:rustc-link-search=native=../native/build/Release");
    println!("cargo:rustc-link-search=native=../native/build");

    // DLL（yamanoha_core.dll, datachannel.dll）を実行ファイルの出力ディレクトリに自動コピーする
    if let Ok(out_dir) = env::var("OUT_DIR") {
        let out_path = Path::new(&out_dir);
        // 通常、OUT_DIR は target/<profile>/build/<package>/out のため、3つ遡ると target/<profile> となる
        if let Some(target_dir) = out_path.ancestors().nth(3) {
            let native_build_dir = Path::new("../native/build");
            
            // コピー対象ファイルリスト (デバッグ用とリリース用)
            let files_to_copy = vec![
                // yamanoha_core.dll
                (native_build_dir.join("Debug/yamanoha_core.dll"), "yamanoha_core.dll"),
                (native_build_dir.join("Release/yamanoha_core.dll"), "yamanoha_core.dll"),
                // datachannel.dll (WebRTCコア)
                (native_build_dir.join("_deps/datachannel-build/Debug/datachannel.dll"), "datachannel.dll"),
                (native_build_dir.join("_deps/datachannel-build/Release/datachannel.dll"), "datachannel.dll"),
            ];

            for (src, dest_name) in files_to_copy {
                if src.exists() {
                    let dest = target_dir.join(dest_name);
                    // コピーを実行し、更新があれば上書きする
                    if let Err(e) = std::fs::copy(&src, &dest) {
                        println!("cargo:warning=Failed to copy DLL from {} to {}: {}", src.display(), dest.display(), e);
                    } else {
                        println!("cargo:warning=Successfully copied {} to {}", src.display(), dest.display());
                    }
                }
            }
        }
    }
    
    tauri_build::build();
}
