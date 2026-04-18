#ifndef POKEWATCHER_VOICE_INPUT_H
#define POKEWATCHER_VOICE_INPUT_H

// Initialize voice input (register double-click polling in renderer loop)
void pw_voice_init(void);

// Call from renderer loop to check for double-click and start recording
void pw_voice_tick(void);

#endif
