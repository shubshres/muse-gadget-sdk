# Direct model providers

Boards with the full Muse UI and PSRAM (`CONFIG_MUSE_HATCH=y`) can use Muse,
Gemini, OpenAI or Anthropic for chat. Settings → Muse → Chat provider cycles
between them. Each provider keeps its own key and model ID across restarts;
switching back to Muse uses the existing Muse pairing and device token.

Upload keys over USB with the board running the updated firmware:

```sh
python3 tools/muse/model_key.py gemini --select
python3 tools/muse/model_key.py openai --select
python3 tools/muse/model_key.py anthropic --select
```

The tool prompts for the key without terminal echo. Do not put keys in command
arguments, source files or chat. Use `--port /dev/cu.usbmodem…` when several
boards are connected. The device confirms NVS persistence before the tool
reports success. Settings shows only whether a key is set.

Each provider has an editable **Model ID** in Settings. You can also set it
while uploading, for example:

```sh
python3 tools/muse/model_key.py openai --model gpt-4.1-mini --select
python3 tools/muse/model_key.py anthropic --clear
python3 tools/muse/model_key.py muse
```

Defaults are `gemini-3.5-flash-lite`, `gpt-4.1-mini` and `claude-sonnet-5-5`.
Gemini Flash-Lite is the lightweight default; Google offers free-tier input
and output for it, subject to your project's eligibility and quota. Billing-enabled
projects follow Google's paid-tier pricing; the device cannot force a project
onto the free tier. Use an API model ID available to your account. Model IDs accept letters,
digits, dots, underscores and hyphens, up to 95 bytes; keys accept up to 511
printable ASCII characters without spaces. Empty key uploads (`--clear`)
remove a provider's key. Empty keyboard input preserves the existing value.

Direct chat uses HTTPS from the device to the provider and the provider's API
billing. It does not use Muse's VM, personal memory, tools or conversation
history: each direct request is independent. Replies are requested with a
2,048-token output budget; the device accepts at most 64 KiB of JSON and
16 KiB of reply text. Voice captions and playback keep the existing 1,023-byte
caption limit. Typed USB chat (`python3 tools/muse/chat.py "hello"`) prints the
full accepted text. No API calls are made just by selecting a provider.

Push-to-talk uses the saved Azure Speech key and region for English (`en-US`)
speech recognition, then sends the transcript to the selected provider. Add
these in Settings → Muse. On the Waveshare 1.75C, the existing Azure speech
playback is reused; other boards show the response as captions. Direct voice
works without Muse pairing once Wi-Fi and the model and Azure keys are set.
Typed USB chat needs only Wi-Fi and the selected model key. Azure and model
API errors show a status or generic error without printing response bodies or
keys. Canceled turns discard late replies; a request already in flight may
finish and incur provider charges. A new request reports busy until that
worker finishes (each HTTPS operation has a 30-second timeout).

Keys use the same NVS storage protection as the existing Azure and Muse
credentials. This feature does not enable flash encryption or Secure Boot.

The implementations follow the official [Gemini generateContent API](https://ai.google.dev/api/generate-content),
[OpenAI Chat Completions API](https://developers.openai.com/api/reference/resources/chat/subresources/completions/methods/create),
[Anthropic Messages API](https://platform.claude.com/docs/en/api/overview), and
[Azure short-audio recognition API](https://learn.microsoft.com/en-us/azure/ai-services/speech-service/rest-speech-to-text-short).

If Gemini reports **NETWORK ERROR (HTTP 0)** on older firmware, the request may
have failed during TLS certificate validation. Google's cross-signed chain needs
`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`; new builds enforce
this setting. Update the active build's configuration and reflash using the
[README procedure](../README.md#update-the-waveshare-175c-and-load-a-gemini-key).
Certificate and hostname verification remain enabled. Updated firmware logs
transport/TLS error codes without credentials or response bodies.
