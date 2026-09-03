// RtAudio common runtime for Emscripten/WebAudio builds.
// Native builds continue to compile RtAudio.cpp unchanged.

#include "RtAudioWeb.h"

#if !defined(__EMSCRIPTEN__)
#error "RtAudioWasm.cpp must only be compiled with Emscripten"
#endif

#include <emscripten/em_macros.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

using namespace rt::audio;

// The WebAudio EM_JS bridge calls these Emscripten JavaScript-library helpers
// indirectly. Emscripten cannot discover such dependencies automatically, so
// declare both explicitly. Keeping the dependency declaration in the common
// WASM runtime makes it travel with the static library for CMake, installed
// target and pkg-config consumers rather than only for repository examples.
EM_JS_DEPS(rtaudio_webaudio_js_deps,
           "$emscriptenRegisterAudioObject,$emscriptenGetAudioObject");

std::shared_ptr<RtApi> createRtApiWebAudio();

const unsigned int RtApi::MAX_SAMPLE_RATES = 14;
const unsigned int RtApi::SAMPLE_RATES[] = {
  4000, 5512, 8000, 9600, 11025, 16000, 22050,
  32000, 44100, 48000, 88200, 96000, 176400, 192000
};

std::string RtAudio::getVersion()
{
  return RTAUDIO_VERSION;
}

extern "C" {
const char *rtaudio_api_names[][2] = {
  { "unspecified", "Unknown" },
  { "core", "CoreAudio" },
  { "alsa", "ALSA" },
  { "jack", "Jack" },
  { "pulse", "Pulse" },
  { "oss", "OpenSoundSystem" },
  { "asio", "ASIO" },
  { "wasapi", "WASAPI" },
  { "ds", "DirectSound" },
  { "dummy", "Dummy" },
  { "webaudio", "WebAudio" },
};

const unsigned int rtaudio_num_api_names =
  sizeof( rtaudio_api_names ) / sizeof( rtaudio_api_names[0] );

const RtAudio::Api rtaudio_compiled_apis[] = {
  RtAudio::WEB_AUDIO,
  RtAudio::UNSPECIFIED,
};

const unsigned int rtaudio_num_compiled_apis =
  sizeof( rtaudio_compiled_apis ) / sizeof( rtaudio_compiled_apis[0] ) - 1;
}

void RtAudio::getCompiledApi( std::vector<RtAudio::Api> &apis )
{
  apis.assign( rtaudio_compiled_apis,
               rtaudio_compiled_apis + rtaudio_num_compiled_apis );
}

std::string RtAudio::getApiName( RtAudio::Api api )
{
  const int index = static_cast<int>( api );
  if ( index < 0 || static_cast<unsigned int>( index ) >= rtaudio_num_api_names )
    return "";
  return rtaudio_api_names[index][0];
}

std::string RtAudio::getApiDisplayName( RtAudio::Api api )
{
  const int index = static_cast<int>( api );
  if ( index < 0 || static_cast<unsigned int>( index ) >= rtaudio_num_api_names )
    return "Unknown";
  return rtaudio_api_names[index][1];
}

RtAudio::Api RtAudio::getCompiledApiByName( const std::string &name )
{
  for ( unsigned int i = 0; i < rtaudio_num_compiled_apis; ++i ) {
    if ( name == rtaudio_api_names[static_cast<int>( rtaudio_compiled_apis[i] )][0] )
      return rtaudio_compiled_apis[i];
  }
  return RtAudio::UNSPECIFIED;
}

RtAudio::Api RtAudio::getCompiledApiByDisplayName( const std::string &name )
{
  for ( unsigned int i = 0; i < rtaudio_num_compiled_apis; ++i ) {
    if ( name == rtaudio_api_names[static_cast<int>( rtaudio_compiled_apis[i] )][1] )
      return rtaudio_compiled_apis[i];
  }
  return RtAudio::UNSPECIFIED;
}

void RtAudio::openRtApi( RtAudio::Api api )
{
  rtapi_.reset();
  if ( api == RtAudio::WEB_AUDIO ) rtapi_ = createRtApiWebAudio();
}

RtAudio::RtAudio( RtAudio::Api api, RtAudioErrorCallback &&errorCallback )
{
  std::string errorMessage;

  if ( api != UNSPECIFIED ) {
    openRtApi( api );
    if ( rtapi_ ) {
      if ( errorCallback ) rtapi_->setErrorCallback( errorCallback );
      return;
    }

    errorMessage = "RtAudio: no compiled support for specified API argument!";
    if ( errorCallback ) errorCallback( RTAUDIO_INVALID_USE, errorMessage );
    else std::cerr << '\n' << errorMessage << '\n' << std::endl;
  }

  std::vector<RtAudio::Api> apis;
  getCompiledApi( apis );
  for ( unsigned int i = 0; i < apis.size(); ++i ) {
    openRtApi( apis[i] );
    if ( rtapi_ && !rtapi_->getDeviceNames().empty() ) break;
  }

  if ( rtapi_ ) {
    if ( errorCallback ) rtapi_->setErrorCallback( errorCallback );
    return;
  }

  errorMessage = "RtAudio: no compiled API support found ... critical error!";
  if ( errorCallback ) errorCallback( RTAUDIO_INVALID_USE, errorMessage );
  else std::cerr << '\n' << errorMessage << '\n' << std::endl;
  std::abort();
}

RtAudioErrorType RtAudio::openStream( RtAudio::StreamParameters *outputParameters,
                                      RtAudio::StreamParameters *inputParameters,
                                      RtAudioFormat format,
                                      unsigned int sampleRate,
                                      unsigned int *bufferFrames,
                                      RtAudioCallback callback,
                                      void *userData,
                                      RtAudio::StreamOptions *options )
{
  return rtapi_->openStream( outputParameters, inputParameters, format,
                             sampleRate, bufferFrames, callback,
                             userData, options );
}

RtApi::RtApi()
{
  clearStreamInfo();
  pthread_mutex_init( &stream_.mutex, nullptr );
  errorCallback_ = 0;
  showWarnings_ = true;
  currentDeviceId_ = 129;
}

RtApi::~RtApi()
{
  pthread_mutex_destroy( &stream_.mutex );
}

RtAudioErrorType RtApi::openStream( RtAudio::StreamParameters *oParams,
                                    RtAudio::StreamParameters *iParams,
                                    RtAudioFormat format,
                                    unsigned int sampleRate,
                                    unsigned int *bufferFrames,
                                    RtAudioCallback callback,
                                    void *userData,
                                    RtAudio::StreamOptions *options )
{
  if ( stream_.state != STREAM_CLOSED ) {
    errorText_ = "RtApi::openStream: a stream is already open!";
    return error( RTAUDIO_INVALID_USE );
  }

  clearStreamInfo();

  if ( oParams && oParams->nChannels < 1 ) {
    errorText_ = "RtApi::openStream: a non-NULL output StreamParameters structure cannot have an nChannels value less than one.";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( iParams && iParams->nChannels < 1 ) {
    errorText_ = "RtApi::openStream: a non-NULL input StreamParameters structure cannot have an nChannels value less than one.";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( !oParams && !iParams ) {
    errorText_ = "RtApi::openStream: input and output StreamParameters structures are both NULL!";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( !bufferFrames ) {
    errorText_ = "RtApi::openStream: bufferFrames is NULL!";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( !callback ) {
    errorText_ = "RtApi::openStream: callback is empty!";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( formatBytes( format ) == 0 ) {
    errorText_ = "RtApi::openStream: 'format' parameter value is undefined.";
    return error( RTAUDIO_INVALID_PARAMETER );
  }

  if ( deviceList_.empty() ) probeDevices();

  unsigned int oChannels = 0;
  if ( oParams ) {
    oChannels = oParams->nChannels;
    bool found = false;
    for ( const auto &device : deviceList_ ) {
      if ( device.ID == oParams->deviceId ) {
        found = true;
        break;
      }
    }
    if ( !found ) {
      errorText_ = "RtApi::openStream: output device ID is invalid.";
      return error( RTAUDIO_INVALID_PARAMETER );
    }
  }

  unsigned int iChannels = 0;
  if ( iParams ) {
    iChannels = iParams->nChannels;
    bool found = false;
    for ( const auto &device : deviceList_ ) {
      if ( device.ID == iParams->deviceId ) {
        found = true;
        break;
      }
    }
    if ( !found ) {
      errorText_ = "RtApi::openStream: input device ID is invalid.";
      return error( RTAUDIO_INVALID_PARAMETER );
    }
  }

  if ( oChannels > 0 &&
       !probeDeviceOpen( oParams->deviceId, OUTPUT, oChannels,
                         oParams->firstChannel, sampleRate, format,
                         bufferFrames, options ) ) {
    return error( RTAUDIO_SYSTEM_ERROR );
  }

  if ( iChannels > 0 &&
       !probeDeviceOpen( iParams->deviceId, INPUT, iChannels,
                         iParams->firstChannel, sampleRate, format,
                         bufferFrames, options ) ) {
    return error( RTAUDIO_SYSTEM_ERROR );
  }

  stream_.callbackInfo.callback = callback;
  stream_.callbackInfo.userData = userData;
  if ( options ) options->numberOfBuffers = stream_.nBuffers;
  stream_.state = STREAM_STOPPED;
  return RTAUDIO_NO_ERROR;
}

void RtApi::probeDevices()
{
}

unsigned int RtApi::getDeviceCount()
{
  probeDevices();
  return static_cast<unsigned int>( deviceList_.size() );
}

std::vector<unsigned int> RtApi::getDeviceIds()
{
  probeDevices();
  std::vector<unsigned int> ids;
  ids.reserve( deviceList_.size() );
  for ( const auto &device : deviceList_ ) ids.push_back( device.ID );
  return ids;
}

std::vector<std::string> RtApi::getDeviceNames()
{
  probeDevices();
  std::vector<std::string> names;
  names.reserve( deviceList_.size() );
  for ( const auto &device : deviceList_ ) names.push_back( device.name );
  return names;
}

unsigned int RtApi::getDefaultInputDevice()
{
  if ( deviceList_.empty() ) probeDevices();
  for ( const auto &device : deviceList_ )
    if ( device.isDefaultInput ) return device.ID;

  for ( auto &device : deviceList_ ) {
    if ( device.inputChannels > 0 ) {
      device.isDefaultInput = true;
      return device.ID;
    }
  }
  return 0;
}

unsigned int RtApi::getDefaultOutputDevice()
{
  if ( deviceList_.empty() ) probeDevices();
  for ( const auto &device : deviceList_ )
    if ( device.isDefaultOutput ) return device.ID;

  for ( auto &device : deviceList_ ) {
    if ( device.outputChannels > 0 ) {
      device.isDefaultOutput = true;
      return device.ID;
    }
  }
  return 0;
}

RtAudio::DeviceInfo RtApi::getDeviceInfo( unsigned int deviceId )
{
  if ( deviceList_.empty() ) probeDevices();
  for ( const auto &device : deviceList_ )
    if ( device.ID == deviceId ) return device;

  errorText_ = "RtApi::getDeviceInfo: deviceId argument not found.";
  error( RTAUDIO_INVALID_PARAMETER );
  return RtAudio::DeviceInfo();
}

void RtApi::closeStream()
{
}

bool RtApi::probeDeviceOpen( unsigned int, StreamMode, unsigned int,
                             unsigned int, unsigned int, RtAudioFormat,
                             unsigned int *, RtAudio::StreamOptions * )
{
  return FAILURE;
}

void RtApi::tickStreamTime()
{
  if ( stream_.sampleRate > 0 )
    stream_.streamTime += stream_.bufferSize * 1.0 / stream_.sampleRate;
}

long RtApi::getStreamLatency()
{
  long totalLatency = 0;
  if ( stream_.mode == OUTPUT || stream_.mode == DUPLEX )
    totalLatency = static_cast<long>( stream_.latency[0] );
  if ( stream_.mode == INPUT || stream_.mode == DUPLEX )
    totalLatency += static_cast<long>( stream_.latency[1] );
  return totalLatency;
}

void RtApi::setStreamTime( double time )
{
  if ( time >= 0.0 ) stream_.streamTime = time;
}

unsigned int RtApi::getStreamSampleRate()
{
  return isStreamOpen() ? stream_.sampleRate : 0;
}

void RtApi::clearStreamInfo()
{
  stream_.mode = UNINITIALIZED;
  stream_.state = STREAM_CLOSED;
  stream_.apiHandle = nullptr;
  stream_.deviceBuffer = nullptr;
  stream_.sampleRate = 0;
  stream_.bufferSize = 0;
  stream_.nBuffers = 0;
  stream_.userInterleaved = true;
  stream_.userFormat = 0;
  stream_.streamTime = 0.0;
  stream_.callbackInfo = CallbackInfo();

  for ( int i = 0; i < 2; ++i ) {
    stream_.deviceId[i] = 0;
    stream_.userBuffer[i] = nullptr;
    stream_.doConvertBuffer[i] = false;
    stream_.deviceInterleaved[i] = true;
    stream_.doByteSwap[i] = false;
    stream_.nUserChannels[i] = 0;
    stream_.nDeviceChannels[i] = 0;
    stream_.channelOffset[i] = 0;
    stream_.latency[i] = 0;
    stream_.deviceFormat[i] = 0;

    stream_.convertInfo[i].channels = 0;
    stream_.convertInfo[i].inJump = 0;
    stream_.convertInfo[i].outJump = 0;
    stream_.convertInfo[i].inFormat = 0;
    stream_.convertInfo[i].outFormat = 0;
    stream_.convertInfo[i].inOffset.clear();
    stream_.convertInfo[i].outOffset.clear();
  }
}

RtAudioErrorType RtApi::error( RtAudioErrorType type )
{
  if ( errorText_.empty() && !errorStream_.str().empty() )
    errorText_ = errorStream_.str();

  errorStream_.str( "" );
  errorStream_.clear();

  if ( type == RTAUDIO_WARNING && !showWarnings_ ) return type;

  if ( errorCallback_ ) {
    errorCallback_( type, errorText_ );
  }
  else if ( type != RTAUDIO_NO_ERROR ) {
    std::cerr << '\n' << errorText_ << '\n' << std::endl;
  }

  return type;
}

unsigned int RtApi::formatBytes( RtAudioFormat format )
{
  if ( format == RTAUDIO_SINT8 ) return 1;
  if ( format == RTAUDIO_SINT16 ) return 2;
  if ( format == RTAUDIO_SINT24 || format == RTAUDIO_SINT32 ||
       format == RTAUDIO_FLOAT32 ) return 4;
  if ( format == RTAUDIO_FLOAT64 ) return 8;
  return 0;
}
