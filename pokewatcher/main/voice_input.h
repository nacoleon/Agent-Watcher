#ifndef POKEWATCHER_VOICE_INPUT_H
#define POKEWATCHER_VOICE_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Initialize voice input (register double-click polling in renderer loop)
void pw_voice_init(void);

// Call from renderer loop to check for double-click and start recording
void pw_voice_tick(void);

// Returns true if a recorded audio buffer is ready for pickup
bool pw_voice_audio_ready(void);

// Get pointer to the WAV audio buffer (header + PCM). Returns NULL if not ready.
const uint8_t *pw_voice_get_audio(size_t *out_size);

// Free the audio buffer after the MCP server has fetched it
void pw_voice_clear_audio(void);

// Returns true if currently recording audio
bool pw_voice_is_recording(void);

// Request early stop of current recording (safe to call from button callback)
void pw_voice_request_stop(void);

#endif
