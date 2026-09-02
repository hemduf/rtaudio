#ifndef RTAUDIO_WEB_H_INCLUDED
#define RTAUDIO_WEB_H_INCLUDED

#include "RtAudio.h"

#if !defined(__EMSCRIPTEN__)
#error "RtAudioWeb.h is only available for Emscripten builds"
#endif

namespace rt {
namespace audio {

// Compatibility alias for code written against the initial WebAudio backend.
// The real API value now lives in RtAudio::Api and remains Emscripten-only, so
// native enum values and native ABI are unchanged.
static const RtAudio::Api RTAUDIO_WEB_AUDIO = RtAudio::WEB_AUDIO;

// Do not automatically connect the RtAudio AudioWorkletNode to the
// AudioContext destination. The node is instead published to JavaScript under
// Module.RtAudioWeb and can be inserted into an arbitrary Web Audio graph.
//
// StreamOptions::streamName is used as the JavaScript lookup name:
//   Module.RtAudioWeb.getNode("my-stream")
//
// The associated context is available as Module.RtAudioWeb.context.
static const RtAudioStreamFlags RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH = 0x40;

} // namespace audio
} // namespace rt

#endif // RTAUDIO_WEB_H_INCLUDED
