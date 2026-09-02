/******************************************/
/*
  webaudio_playsaw.cpp

  Browser/WebAudio counterpart to playsaw.cpp.
  The stream is opened and started from an exported function so that
  AudioContext.resume() runs directly from a browser user gesture.

  Frequency and gain are updated from the browser main thread and read
  atomically by the AudioWorklet callback to demonstrate realtime-safe
  communication with the backend. The callback publishes the last values it
  actually consumed as well as stereo peak levels so the HTML page can show
  communication in both directions between the UI and the audio thread.

  The stream uses RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH so the browser explicitly
  connects the published AudioWorkletNode into its Web Audio graph.
*/
/******************************************/

#include "RtAudioWeb.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>

namespace {

constexpr unsigned int kChannels = 2;
constexpr unsigned int kRequestedSampleRate = 48000;
constexpr unsigned int kDefaultFrequencyHz = 220;
constexpr unsigned int kMinFrequencyHz = 20;
constexpr unsigned int kMaxFrequencyHz = 5000;
constexpr unsigned int kGainScale = 1000000;
constexpr unsigned int kDefaultGainFixed = 250000;
constexpr unsigned int kMeterScale = 1000000;

struct SawState {
  std::array<double, kChannels> phase{{0.0, 0.0}};

  // Browser main thread -> AudioWorklet targets. 32-bit integer atomics map
  // directly to Wasm atomics when shared memory is enabled.
  std::atomic<unsigned int> targetFrequencyHz{kDefaultFrequencyHz};
  std::atomic<unsigned int> targetGainFixed{kDefaultGainFixed};

  // AudioWorklet -> browser main thread acknowledgements. These are written
  // only after the realtime callback has consumed the corresponding targets.
  std::atomic<unsigned int> appliedFrequencyHz{kDefaultFrequencyHz};
  std::atomic<unsigned int> appliedGainFixed{kDefaultGainFixed};

  // AudioWorklet -> browser main thread stereo peak meters. The callback
  // publishes one linear 0..1 peak value per channel after each render quantum.
  std::atomic<unsigned int> meterLeftFixed{0};
  std::atomic<unsigned int> meterRightFixed{0};

  // Audio-thread-only smoothed values.
  double currentFrequencyHz{static_cast<double>( kDefaultFrequencyHz )};
  double currentGain{static_cast<double>( kDefaultGainFixed ) / kGainScale};
  unsigned int sampleRate{kRequestedSampleRate};
};

std::unique_ptr<RtAudio> audio;
SawState sawState;
unsigned int activeSampleRate = 0;
unsigned int activeBufferFrames = 0;

void errorCallback( RtAudioErrorType /*type*/, const std::string &errorText )
{
  std::cerr << "RtAudio WebAudio: " << errorText << std::endl;
}

unsigned int levelToFixed( double level )
{
  const double clamped = std::max( 0.0, std::min( level, 1.0 ) );
  return static_cast<unsigned int>( clamped * kMeterScale + 0.5 );
}

int saw( void *outputBuffer, void * /*inputBuffer*/,
         unsigned int nBufferFrames, double /*streamTime*/,
         RtAudioStreamStatus /*status*/, void *data )
{
  float *buffer = static_cast<float *>( outputBuffer );
  SawState *state = static_cast<SawState *>( data );

  // Read UI targets once per WebAudio render quantum. The callback performs no
  // logging, allocation or locking on the realtime AudioWorklet thread.
  const unsigned int frequencyHz =
    state->targetFrequencyHz.load( std::memory_order_relaxed );
  const unsigned int gainFixed =
    state->targetGainFixed.load( std::memory_order_relaxed );

  // Acknowledge from the actual audio thread. The HTML page polls these values
  // to demonstrate that the AudioWorklet consumed the UI update.
  state->appliedFrequencyHz.store( frequencyHz, std::memory_order_relaxed );
  state->appliedGainFixed.store( gainFixed, std::memory_order_relaxed );

  const double targetFrequencyHz = static_cast<double>( frequencyHz );
  const double targetGain = static_cast<double>( gainFixed ) / kGainScale;
  const double frames = nBufferFrames > 0 ? static_cast<double>( nBufferFrames ) : 1.0;
  const double frequencyStep = ( targetFrequencyHz - state->currentFrequencyHz ) / frames;
  const double gainStep = ( targetGain - state->currentGain ) / frames;
  const double inverseSampleRate = 1.0 / static_cast<double>( state->sampleRate );
  double peak[kChannels] = { 0.0, 0.0 };

  for ( unsigned int frame = 0; frame < nBufferFrames; ++frame ) {
    state->currentFrequencyHz += frequencyStep;
    state->currentGain += gainStep;

    const double increment[kChannels] = {
      2.0 * state->currentFrequencyHz * inverseSampleRate,
      2.0 * state->currentFrequencyHz * 1.5 * inverseSampleRate
    };

    for ( unsigned int channel = 0; channel < kChannels; ++channel ) {
      const float sample =
        static_cast<float>( state->phase[channel] * state->currentGain );
      buffer[frame * kChannels + channel] = sample;

      const double magnitude = std::abs( static_cast<double>( sample ) );
      if ( magnitude > peak[channel] ) peak[channel] = magnitude;

      state->phase[channel] += increment[channel];
      if ( state->phase[channel] >= 1.0 )
        state->phase[channel] -= 2.0;
    }
  }

  // Publish meters once per render quantum after all samples have been written.
  // These relaxed atomic stores are the complete audio-thread -> UI transport.
  state->meterLeftFixed.store( levelToFixed( peak[0] ), std::memory_order_relaxed );
  state->meterRightFixed.store( levelToFixed( peak[1] ), std::memory_order_relaxed );

  return 0;
}

void resetSaw( unsigned int sampleRate )
{
  sawState.phase = {{0.0, 0.0}};
  sawState.sampleRate = sampleRate;

  const unsigned int frequencyHz =
    sawState.targetFrequencyHz.load( std::memory_order_relaxed );
  const unsigned int gainFixed =
    sawState.targetGainFixed.load( std::memory_order_relaxed );

  sawState.currentFrequencyHz = static_cast<double>( frequencyHz );
  sawState.currentGain = static_cast<double>( gainFixed ) / kGainScale;
  sawState.meterLeftFixed.store( 0, std::memory_order_relaxed );
  sawState.meterRightFixed.store( 0, std::memory_order_relaxed );
}

void destroyStream()
{
  sawState.meterLeftFixed.store( 0, std::memory_order_relaxed );
  sawState.meterRightFixed.store( 0, std::memory_order_relaxed );

  if ( !audio ) return;

  if ( audio->isStreamRunning() )
    audio->stopStream();
  if ( audio->isStreamOpen() )
    audio->closeStream();

  audio.reset();
  activeSampleRate = 0;
  activeBufferFrames = 0;
}

unsigned int clampFrequency( float value )
{
  const float clamped = std::max(
    static_cast<float>( kMinFrequencyHz ),
    std::min( value, static_cast<float>( kMaxFrequencyHz ) ) );
  return static_cast<unsigned int>( clamped + 0.5f );
}

unsigned int clampGain( float value )
{
  const float clamped = std::max( 0.0f, std::min( value, 1.0f ) );
  return static_cast<unsigned int>( clamped * kGainScale + 0.5f );
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
int rtaudio_webaudio_playsaw_start()
{
  if ( audio && audio->isStreamRunning() )
    return 0;

  if ( !audio ) {
    // The browser page has already awaited Module.RtAudioWeb.initialize().
    // WebAudio is therefore a normal synchronous RtAudio API from this point.
    audio.reset( new RtAudio( RtAudio::WEB_AUDIO, &errorCallback ) );
    audio->showWarnings( true );

    const unsigned int deviceId = audio->getDefaultOutputDevice();
    if ( deviceId == 0 ) {
      std::cerr << "No WebAudio output device found" << std::endl;
      destroyStream();
      return 1;
    }

    RtAudio::StreamParameters outputParameters;
    outputParameters.deviceId = deviceId;
    outputParameters.nChannels = kChannels;
    outputParameters.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY |
                    RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH;
    options.streamName = "rtaudio-playsaw";

    unsigned int bufferFrames = 0;
    if ( audio->openStream( &outputParameters, nullptr, RTAUDIO_FLOAT32,
                            kRequestedSampleRate, &bufferFrames, &saw,
                            &sawState, &options ) != RTAUDIO_NO_ERROR ) {
      std::cerr << audio->getErrorText() << std::endl;
      destroyStream();
      return 1;
    }

    activeSampleRate = audio->getStreamSampleRate();
    activeBufferFrames = bufferFrames;
    if ( activeSampleRate == 0 ) activeSampleRate = kRequestedSampleRate;
    resetSaw( activeSampleRate );
  }

  // This function is called synchronously by the HTML click handler. That is
  // important: browsers only allow AudioContext.resume() from a user gesture.
  if ( audio->startStream() != RTAUDIO_NO_ERROR ) {
    std::cerr << audio->getErrorText() << std::endl;
    return 2;
  }

  return 0;
}

EMSCRIPTEN_KEEPALIVE
void rtaudio_webaudio_playsaw_stop()
{
  sawState.meterLeftFixed.store( 0, std::memory_order_relaxed );
  sawState.meterRightFixed.store( 0, std::memory_order_relaxed );

  // Keep the AudioWorkletNode and graph alive across demo Start/Stop cycles.
  // Besides avoiding unnecessary browser graph churn, this also avoids
  // accumulating retired callback userdata on Emscripten versions whose node
  // destruction API has no completion fence.
  if ( audio && audio->isStreamRunning() )
    audio->stopStream();
}

EMSCRIPTEN_KEEPALIVE
void rtaudio_webaudio_playsaw_set_frequency( float frequency )
{
  sawState.targetFrequencyHz.store(
    clampFrequency( frequency ), std::memory_order_relaxed );
}

EMSCRIPTEN_KEEPALIVE
void rtaudio_webaudio_playsaw_set_gain( float gain )
{
  sawState.targetGainFixed.store(
    clampGain( gain ), std::memory_order_relaxed );
}

EMSCRIPTEN_KEEPALIVE
float rtaudio_webaudio_playsaw_frequency()
{
  return static_cast<float>(
    sawState.appliedFrequencyHz.load( std::memory_order_relaxed ) );
}

EMSCRIPTEN_KEEPALIVE
float rtaudio_webaudio_playsaw_gain()
{
  return static_cast<float>(
    sawState.appliedGainFixed.load( std::memory_order_relaxed ) ) / kGainScale;
}

EMSCRIPTEN_KEEPALIVE
float rtaudio_webaudio_playsaw_meter_left()
{
  return static_cast<float>(
    sawState.meterLeftFixed.load( std::memory_order_relaxed ) ) / kMeterScale;
}

EMSCRIPTEN_KEEPALIVE
float rtaudio_webaudio_playsaw_meter_right()
{
  return static_cast<float>(
    sawState.meterRightFixed.load( std::memory_order_relaxed ) ) / kMeterScale;
}

EMSCRIPTEN_KEEPALIVE
int rtaudio_webaudio_playsaw_is_running()
{
  return audio && audio->isStreamRunning() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
unsigned int rtaudio_webaudio_playsaw_sample_rate()
{
  return activeSampleRate;
}

EMSCRIPTEN_KEEPALIVE
unsigned int rtaudio_webaudio_playsaw_buffer_frames()
{
  return activeBufferFrames;
}

} // extern "C"

int main()
{
  std::cout << "RtAudio WebAudio playsaw ready" << std::endl;
  return 0;
}
