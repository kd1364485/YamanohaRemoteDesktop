import { dom } from "./dom";

// 監視ステータスバーを更新
export function updateWatchStatus(status: "watching" | "stopped" | "not-watching") {
  if (!dom.statusWatch) return;
  const dot = dom.statusWatch.querySelector("span") as HTMLElement;
  if (!dot) return;

  if (status === "watching") {
    dot.classList.remove("bg-gray-500", "bg-red-500");
    dot.classList.add("bg-green-500");
    dom.statusWatch.innerHTML = `<span class="w-2 h-2 rounded-full bg-green-500"></span> 監視中`;
  } else if (status === "stopped") {
    dot.classList.remove("bg-gray-500", "bg-green-500");
    dot.classList.add("bg-red-500");
    dom.statusWatch.innerHTML = `<span class="w-2 h-2 rounded-full bg-red-500"></span> 監視停止`;
  } else {
    dot.classList.remove("bg-green-500", "bg-red-500");
    dot.classList.add("bg-gray-500");
    dom.statusWatch.innerHTML = `<span class="w-2 h-2 rounded-full bg-gray-500"></span> 未監視`;
  }
}

// インデックスステータスバーを更新
export function updateIndexStatus(status: "indexed" | "not-indexed" | "updating" | "updated") {
  if (!dom.statusIndex) return;
  const dot = dom.statusIndex.querySelector("span") as HTMLElement;
  if (!dot) return;

  if (status === "indexed") {
    dot.classList.remove("bg-gray-500", "bg-yellow-500");
    dot.classList.add("bg-green-500");
    dom.statusIndex.innerHTML = `<span class="w-2 h-2 rounded-full bg-green-500"></span> インデックスあり`;
  } else if (status === "not-indexed") {
    dot.classList.remove("bg-green-500", "bg-yellow-500");
    dot.classList.add("bg-gray-500");
    dom.statusIndex.innerHTML = `<span class="w-2 h-2 rounded-full bg-gray-500"></span> 未インデックス`;
  } else if (status === "updating") {
    dot.classList.remove("bg-gray-500", "bg-green-500");
    dot.classList.add("bg-yellow-500");
    dom.statusIndex.innerHTML = `<span class="w-2 h-2 rounded-full bg-yellow-500 animate-pulse"></span> 更新中...`;
  } else if (status === "updated") {
    dot.classList.remove("bg-gray-500", "bg-yellow-500");
    dot.classList.add("bg-green-500");
    dom.statusIndex.innerHTML = `<span class="w-2 h-2 rounded-full bg-green-500"></span> 更新完了 ✅`;
    setTimeout(() => {
      updateIndexStatus("indexed");
    }, 2000);
  }
}

// 選択中パスのステータス表示を更新
export function updateSelectedPathStatus(path: string) {
  if (dom.selectedPathStatus) {
    dom.selectedPathStatus.textContent = `選択されたパス: ${path || "なし"}`;
  }
}
