export const API_KEY_STORAGE_KEY = "gemini_api_key";
export const PROMPT_STORAGE_KEY = "gemini_prompt";
export const MODEL_STORAGE_KEY = "gemini_model";
export const PATH_STORAGE_KEY = "selected_path";

export const DEFAULT_PROMPT = `あなたは神レベルのプロのエンジニアであり、ゲーム開発の専門家です。以下のプロジェクト情報を参考に、質問に答えてください

プロジェクト概要:
{overview}

ファイルツリー:
{file_tree}

ソースコード:
{files_content}

ユーザーの質問:
{message}`;
