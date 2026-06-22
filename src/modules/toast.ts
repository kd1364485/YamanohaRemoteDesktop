import { dom } from "./dom";

// トースト通知を画面右上に表示する
export function showToast(
  message: string,
  type: "info" | "success" | "error" = "info",
  duration: number = 3000
) {
  if (!dom.toastContainer) return;

  const toast = document.createElement("div");
  const bgColor =
    type === "success" ? "bg-green-600" : type === "error" ? "bg-red-600" : "bg-blue-600";
  const icon = type === "success" ? "✅" : type === "error" ? "❌" : "ℹ️";

  toast.className = `${bgColor} text-white px-4 py-3 rounded-lg shadow-lg flex items-center gap-2 min-w-72 transform transition-all duration-300 translate-x-full opacity-0`;
  toast.innerHTML = `<span>${icon}</span><span>${message}</span>`;

  dom.toastContainer.appendChild(toast);

  // アニメーションで表示
  requestAnimationFrame(() => {
    toast.classList.remove("translate-x-full", "opacity-0");
  });

  // 指定時間後に削除
  setTimeout(() => {
    toast.classList.add("translate-x-full", "opacity-0");
    setTimeout(() => {
      toast.remove();
    }, 300);
  }, duration);
}
