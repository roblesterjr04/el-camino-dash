#pragma once

// The few commands the ESP32 recognises itself (esp-sr MultiNet), so the driver can turn the volume down while the
// compute module is still booting or has stopped. They are sent straight from the card, as the audio card's sink
// understands them (AUDIO_CMD); they do not go through the AI policy because the module is not involved.

#include <stddef.h>
#include <stdint.h>

#include <elc_chan.h>
#include <elc_chan_msg.h>

enum AiCommand : uint8_t {
  AI_CMD_NONE = 0, AI_CMD_VOLUME_UP, AI_CMD_VOLUME_DOWN, AI_CMD_MUTE, AI_CMD_NEXT, AI_CMD_PREVIOUS, AI_CMD_PAUSE_PLAY,
  AI_CMD_COUNT
};

struct AiCommandPhrase {
  AiCommand id;
  const char *phrase;  // what MultiNet is taught
};
extern const AiCommandPhrase kAiCommandPhrases[];
extern const size_t kAiCommandPhraseCount;

/** The AUDIO_CMD transfer for a command to `sink` (the audio card's node). False for an unknown command. */
bool ai_command_build(AiCommand c, uint8_t sink, ElcChWriter &w);
