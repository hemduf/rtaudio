#include "RtAudioWeb.h"
#include "rtaudio_c.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

int main()
{
#if defined(__EMSCRIPTEN__)
  static_assert( RtAudio::WEB_AUDIO < RtAudio::NUM_APIS,
                 "WebAudio must be a real RtAudio API before NUM_APIS" );
  static_assert( RTAUDIO_WEB_AUDIO == RtAudio::WEB_AUDIO,
                 "RtAudioWeb.h compatibility alias must match RtAudio::WEB_AUDIO" );
#endif

  const RtAudio::Api webAudio = RtAudio::getCompiledApiByName( "webaudio" );
  if ( webAudio == RtAudio::UNSPECIFIED ) {
    std::cerr << "WebAudio API lookup failed\n";
    return 1;
  }

#if defined(__EMSCRIPTEN__)
  if ( webAudio != RtAudio::WEB_AUDIO ) {
    std::cerr << "WebAudio name lookup does not return RtAudio::WEB_AUDIO\n";
    return 2;
  }
#endif

  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi( apis );
  if ( std::find( apis.begin(), apis.end(), webAudio ) == apis.end() ) {
    std::cerr << "WebAudio is not in the compiled API list\n";
    return 3;
  }

  if ( RtAudio::getApiName( webAudio ) != "webaudio" ||
       RtAudio::getApiDisplayName( webAudio ) != "WebAudio" ) {
    std::cerr << "Unexpected WebAudio API name\n";
    return 4;
  }

#if defined(__EMSCRIPTEN__)
  if ( rtaudio_compiled_api_by_name( "webaudio" ) != RTAUDIO_API_WEB_AUDIO ||
       std::strcmp( rtaudio_api_name( RTAUDIO_API_WEB_AUDIO ), "webaudio" ) != 0 ||
       std::strcmp( rtaudio_api_display_name( RTAUDIO_API_WEB_AUDIO ), "WebAudio" ) != 0 ||
       RTAUDIO_FLAGS_WEB_AUDIO_EXTERNAL_GRAPH != RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH ) {
    std::cerr << "Unexpected WebAudio C API mapping\n";
    return 5;
  }
#endif

  RtAudio audio( webAudio );
  if ( audio.getCurrentApi() != webAudio ) {
    std::cerr << "WebAudio backend was not selected\n";
    return 6;
  }

  if ( audio.getDeviceCount() != 1 ) {
    std::cerr << "WebAudio must expose exactly one virtual output device\n";
    return 7;
  }

  const unsigned int deviceId = audio.getDefaultOutputDevice();
  if ( deviceId == 0 || audio.getDefaultInputDevice() != 0 ) {
    std::cerr << "Unexpected default WebAudio device selection\n";
    return 8;
  }

  const RtAudio::DeviceInfo info = audio.getDeviceInfo( deviceId );
  if ( info.outputChannels < 2 || info.inputChannels != 0 ||
       !info.isDefaultOutput || info.isDefaultInput ||
       info.nativeFormats != RTAUDIO_FLOAT32 ) {
    std::cerr << "Unexpected WebAudio device capabilities\n";
    return 9;
  }

  return 0;
}
