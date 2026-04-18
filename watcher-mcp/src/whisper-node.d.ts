declare module "whisper-node" {
  interface WhisperResult {
    start: string;
    end: string;
    speech: string;
  }
  interface WhisperOptions {
    modelName: string;
    whisperOptions?: { language?: string };
  }
  export function whisper(
    filePath: string,
    options: WhisperOptions
  ): Promise<WhisperResult[]>;
}
