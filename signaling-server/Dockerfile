# better-sqlite3 はネイティブモジュールなのでビルドツールが必要
FROM node:20-slim

WORKDIR /app

# better-sqlite3 のビルドに必要なツール（prebuilt が使えない場合の保険）
RUN apt-get update \
  && apt-get install -y --no-install-recommends python3 make g++ \
  && rm -rf /var/lib/apt/lists/*

# 依存関係を先にインストール（レイヤーキャッシュを効かせる）
COPY package*.json ./
RUN npm install --omit=dev

# アプリ本体をコピー
COPY . .

ENV PORT=8080
EXPOSE 8080

CMD ["node", "server.js"]
