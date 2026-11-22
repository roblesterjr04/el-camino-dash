#include "ai_commands.h"

const AiCommandPhrase kAiCommandPhrases[] = {
    {AI_CMD_VOLUME_UP, "turn it up"},        {AI_CMD_VOLUME_UP, "volume up"},
    {AI_CMD_VOLUME_DOWN, "turn it down"},    {AI_CMD_VOLUME_DOWN, "volume down"},
    {AI_CMD_MUTE, "mute"},                   {AI_CMD_MUTE, "be quiet"},
    {AI_CMD_NEXT, "next song"},              {AI_CMD_PREVIOUS, "previous song"},
    {AI_CMD_PAUSE_PLAY, "pause"},            {AI_CMD_PAUSE_PLAY, "play music"},
};
const size_t kAiCommandPhraseCount = sizeof(kAiCommandPhrases) / sizeof(kAiCommandPhrases[0]);

bool ai_command_build(AiCommand c, uint8_t sink, ElcChWriter &w) {
  switch (c) {
    case AI_CMD_VOLUME_UP: elc_audio_encode(ElcAudioVolume{sink, 1, 5}, w); return true;
    case AI_CMD_VOLUME_DOWN: elc_audio_encode(ElcAudioVolume{sink, 1, -5}, w); return true;
    case AI_CMD_MUTE: elc_audio_encode(ElcAudioVolume{sink, 2, 0}, w); return true;
    case AI_CMD_NEXT: elc_audio_encode(ElcAudioMedia{sink, ELC_MEDIA_NEXT, 0}, w); return true;
    case AI_CMD_PREVIOUS: elc_audio_encode(ElcAudioMedia{sink, ELC_MEDIA_PREV, 0}, w); return true;
    case AI_CMD_PAUSE_PLAY: elc_audio_encode(ElcAudioMedia{sink, ELC_MEDIA_TOGGLE, 0}, w); return true;
    default: return false;
  }
}
