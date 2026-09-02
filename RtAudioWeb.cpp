#include "RtAudioWeb.h"

#if defined(__RTAUDIO_WEB_AUDIO__)

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace rt::audio;

EM_JS(void, rtaudio_web_install_api, (), {
  const api = Module.RtAudioWeb || (Module.RtAudioWeb = {});
  api.nodes = api.nodes || new Map();
  api.namedNodes = api.namedNodes || new Map();
  api.context = api.context || null;
  api._contextHandle = api._contextHandle || 0;
  api._state = api._state || 'uninitialized';
  api._initializePromise = api._initializePromise || null;
  api._resolveInitialize = null;
  api._rejectInitialize = null;

  api.getNode = function(key) {
    if (typeof key === 'string') return api.namedNodes.get(key);
    return api.nodes.get(key);
  };
  api.getContext = function() { return api.context; };
  api.isReady = function() { return api._state === 'ready'; };
  api.connect = function(key, destination, output, input) {
    const audioNode = api.getNode(key);
    if (!audioNode) throw new Error('Unknown RtAudio WebAudio node: ' + key);
    audioNode.connect(destination,
                      output === undefined ? 0 : output,
                      input === undefined ? 0 : input);
    return audioNode;
  };
  api.disconnect = function(key) {
    const audioNode = api.getNode(key);
    if (!audioNode) return;
    audioNode.disconnect();
  };

  api._completeInitialization = function(success, message) {
    const resolve = api._resolveInitialize;
    const reject = api._rejectInitialize;
    api._resolveInitialize = null;
    api._rejectInitialize = null;

    if (success) {
      api._state = 'ready';
      if (typeof resolve === 'function') resolve(api);
    } else {
      api._state = 'failed';
      api._initializePromise = null;
      if (typeof reject === 'function')
        reject(new Error(message || 'RtAudio WebAudio initialization failed'));
    }
  };

  api.initialize = function(options) {
    options = options || {};

    const AudioContextType = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!AudioContextType)
      return Promise.reject(new Error('Web Audio AudioContext is unavailable'));

    let context = options.context || null;
    if (!context) {
      const attributes = { latencyHint: options.latencyHint || 'interactive' };
      if (options.sampleRate) attributes.sampleRate = options.sampleRate;
      context = new AudioContextType(attributes);
    }

    if (!(context instanceof AudioContextType))
      return Promise.reject(new TypeError('RtAudioWeb.initialize({context}) requires an AudioContext'));

    if (api._state === 'ready') {
      if (api.context !== context)
        return Promise.reject(new Error('RtAudio WebAudio is already initialized with another AudioContext'));
      return Promise.resolve(api);
    }

    if (api._state === 'initializing') {
      if (api.context !== context)
        return Promise.reject(new Error('RtAudio WebAudio is already initializing with another AudioContext'));
      return api._initializePromise;
    }

    if (api._state === 'failed')
      return Promise.reject(new Error('RtAudio WebAudio initialization previously failed'));

    const handle = emscriptenRegisterAudioObject(context);
    if (!handle)
      return Promise.reject(new Error('Unable to register the host AudioContext with Emscripten'));

    api.context = context;
    api._contextHandle = handle;
    api._state = 'initializing';
    api._initializePromise = new Promise(function(resolve, reject) {
      api._resolveInitialize = resolve;
      api._rejectInitialize = reject;
    });

    const result = Module._rtaudio_web_initialize_context(handle);
    if (result !== 0) {
      api._completeInitialization(false,
        'RtAudio rejected the WebAudio initialization request');
    }

    return api._initializePromise;
  };
});

EM_JS(void, rtaudio_web_complete_initialization,
      (int success, const char *messagePtr), {
  const api = Module.RtAudioWeb;
  if (!api || typeof api._completeInitialization !== 'function') return;
  const message = messagePtr ? UTF8ToString(messagePtr) : String();
  api._completeInitialization(!!success, message);
});

EM_JS(void, rtaudio_web_publish_node,
      (EMSCRIPTEN_WEBAUDIO_T nodeHandle,
       EMSCRIPTEN_WEBAUDIO_T contextHandle,
       const char *namePtr,
       int externalGraph), {
  const node = emscriptenGetAudioObject(nodeHandle);
  const context = emscriptenGetAudioObject(contextHandle);
  const name = namePtr ? UTF8ToString(namePtr) : String();

  const api = Module.RtAudioWeb || (Module.RtAudioWeb = {});
  api.nodes = api.nodes || new Map();
  api.namedNodes = api.namedNodes || new Map();
  api.context = context;

  api.nodes.set(nodeHandle, node);
  if (name) api.namedNodes.set(name, node);

  // Do not invoke arbitrary application JavaScript while C++ is still inside
  // openStream()/createNode(). A synchronous callback could close the stream
  // and free its WebAudioHandle before the native call unwinds. The registry is
  // updated immediately, while user notification is deferred to a microtask.
  const callback = api.onNodeCreated;
  if (typeof callback === 'function') {
    const info = {
      id: nodeHandle,
      name: name,
      node: node,
      context: context,
      externalGraph: !!externalGraph
    };
    const notify = function() {
      // If the stream was closed before this microtask ran, suppress the stale
      // creation notification. Destruction still has its own notification.
      if (api.nodes && api.nodes.get(nodeHandle) === node)
        callback(info);
    };
    if (typeof queueMicrotask === 'function') queueMicrotask(notify);
    else Promise.resolve().then(notify);
  }
});

EM_JS(void, rtaudio_web_unpublish_node,
      (EMSCRIPTEN_WEBAUDIO_T nodeHandle, const char *namePtr), {
  const api = Module.RtAudioWeb;
  if (!api) return;

  const name = namePtr ? UTF8ToString(namePtr) : String();
  const node = api.nodes ? api.nodes.get(nodeHandle) : undefined;
  const callback = api.onNodeDestroyed;

  if (api.nodes) api.nodes.delete(nodeHandle);
  if (api.namedNodes && name && api.namedNodes.get(name) === node)
    api.namedNodes.delete(name);

  if (typeof callback === 'function') {
    const info = { id: nodeHandle, name: name, node: node };
    const notify = function() { callback(info); };
    if (typeof queueMicrotask === 'function') queueMicrotask(notify);
    else Promise.resolve().then(notify);
  }
});

namespace {

constexpr unsigned int kWebAudioDeviceId = 1;
constexpr unsigned int kMaxWebAudioChannels = 2;
constexpr std::size_t kAudioWorkletStackSize = 64 * 1024;
constexpr const char *kProcessorName = "rtaudio-webaudio";

class RtApiWebAudio;

struct WebAudioHandle {
  EMSCRIPTEN_WEBAUDIO_T node{0};
  std::atomic<RtApiWebAudio *> owner{nullptr};
  std::atomic<unsigned int> activeCallbacks{0};
  unsigned int channels{0};
  bool externalGraph{false};
  std::string name;
};

class WebAudioRuntime final
{
public:
  static WebAudioRuntime &instance()
  {
    // The Wasm AudioWorklet scope has page lifetime and Emscripten 6.0.5 does
    // not provide a fenced node-destruction API. Intentionally never destroy
    // this runtime so callback userData retained below remains valid until the
    // browser tears the page down.
    static WebAudioRuntime *runtime = new WebAudioRuntime;
    return *runtime;
  }

  bool initialize( EMSCRIPTEN_WEBAUDIO_T context, std::string &errorText );
  bool attach( WebAudioHandle *handle, std::string &errorText );
  void detach( WebAudioHandle *handle );
  static void synchronizeStoppedStreamsOnMainThread();

  EMSCRIPTEN_WEBAUDIO_T context() const { return context_; }
  unsigned int sampleRate() const { return sampleRate_; }
  unsigned int quantumSize() const { return quantumSize_; }
  bool ready() const { return ready_; }
  bool failed() const { return failed_; }

private:
  WebAudioRuntime() = default;
  ~WebAudioRuntime() = default;
  WebAudioRuntime( const WebAudioRuntime & ) = delete;
  WebAudioRuntime &operator=( const WebAudioRuntime & ) = delete;

  static void workletThreadStarted( EMSCRIPTEN_WEBAUDIO_T context,
                                    bool success, void *userData );
  static void processorCreated( EMSCRIPTEN_WEBAUDIO_T context,
                                bool success, void *userData );
  static bool process( int numInputs, const AudioSampleFrame *inputs,
                       int numOutputs, AudioSampleFrame *outputs,
                       int numParams, const AudioParamFrame *params,
                       void *userData );

  bool createNode( WebAudioHandle *handle, std::string &errorText );
  void fail( const char *message );

  EMSCRIPTEN_WEBAUDIO_T context_{0};
  unsigned int sampleRate_{0};
  unsigned int quantumSize_{128};
  bool initializing_{false};
  bool ready_{false};
  bool failed_{false};
  alignas(16) unsigned char workletStack_[kAudioWorkletStackSize]{};
  std::vector<WebAudioHandle *> handles_;

  // emscripten_destroy_web_audio_node() disconnects/removes the JS node but, in
  // current Emscripten, does not guarantee that process() can never be invoked
  // again with the registered userData pointer. Retain detached handles for the
  // lifetime of the page. Their owner is null, so any late callback only zeros
  // its output and never reaches a freed RtApiWebAudio/user buffer.
  std::vector<std::unique_ptr<WebAudioHandle>> retiredHandles_;
};

class RtApiWebAudio final : public RtApi
{
public:
  RtApiWebAudio() = default;

  ~RtApiWebAudio() override
  {
    if ( stream_.state != STREAM_CLOSED ) closeStream();
  }

  RtAudio::Api getCurrentApi() override { return RtAudio::WEB_AUDIO; }
  void closeStream() override;
  RtAudioErrorType startStream() override;
  RtAudioErrorType stopStream() override;
  RtAudioErrorType abortStream() override;
  double getStreamTime() const override;
  void setStreamTime( double time ) override;

private:
  friend class WebAudioRuntime;

  std::atomic<bool> running_{false};
  std::atomic<bool> initFailed_{false};
  std::atomic<std::uint64_t> streamFrames_{0};

  void probeDevices() override;
  bool probeDeviceOpen( unsigned int deviceId, StreamMode mode,
                        unsigned int channels, unsigned int firstChannel,
                        unsigned int sampleRate, RtAudioFormat format,
                        unsigned int *bufferSize,
                        RtAudio::StreamOptions *options ) override;

  void reportAsyncFailure( const char *message );
  void callbackEvent( AudioSampleFrame &output );
  float sampleAsFloat( unsigned int frame, unsigned int channel ) const;
  void synchronizeStoppedStateOnMainThread()
  {
    if ( !running_.load( std::memory_order_acquire ) &&
         stream_.state == STREAM_RUNNING ) {
      stream_.state = STREAM_STOPPED;
    }
  }
  WebAudioHandle *webHandle() const
  {
    return static_cast<WebAudioHandle *>( stream_.apiHandle );
  }
  void waitForActiveCallback() const
  {
    WebAudioHandle *handle = webHandle();
    while ( handle &&
            handle->activeCallbacks.load( std::memory_order_acquire ) != 0 ) {}
  }
};

struct WebAudioJsApiInstaller {
  WebAudioJsApiInstaller() { rtaudio_web_install_api(); }
};

WebAudioJsApiInstaller gWebAudioJsApiInstaller;

bool WebAudioRuntime::initialize( EMSCRIPTEN_WEBAUDIO_T context,
                                  std::string &errorText )
{
  if ( !emscripten_is_main_browser_thread() ) {
    errorText = "RtAudio WebAudio initialization must run on the browser main thread.";
    return false;
  }

  if ( failed_ ) {
    errorText = "RtAudio WebAudio initialization previously failed.";
    return false;
  }

  if ( context <= 0 ) {
    errorText = "RtAudio WebAudio initialization received an invalid AudioContext handle.";
    return false;
  }

  if ( ready_ ) {
    if ( context_ != context ) {
      errorText = "RtAudio WebAudio is already initialized with another AudioContext.";
      return false;
    }
    return true;
  }

  if ( initializing_ ) {
    if ( context_ != context ) {
      errorText = "RtAudio WebAudio is already initializing with another AudioContext.";
      return false;
    }
    return true;
  }

  context_ = context;

  int actualSampleRate = emscripten_audio_context_sample_rate( context_ );
  if ( actualSampleRate <= 0 ) {
    errorText = "RtAudio WebAudio could not query the host AudioContext sample rate.";
    context_ = 0;
    return false;
  }
  sampleRate_ = static_cast<unsigned int>( actualSampleRate );

  int quantum = emscripten_audio_context_quantum_size( context_ );
  if ( quantum <= 0 ) quantum = 128;
  quantumSize_ = static_cast<unsigned int>( quantum );

  initializing_ = true;
  emscripten_start_wasm_audio_worklet_thread_async(
    context_, workletStack_, sizeof( workletStack_ ),
    &WebAudioRuntime::workletThreadStarted, this );
  return true;
}

bool WebAudioRuntime::attach( WebAudioHandle *handle, std::string &errorText )
{
  if ( !handle ) {
    errorText = "RtApiWebAudio: invalid stream handle.";
    return false;
  }
  if ( failed_ ) {
    errorText = "RtApiWebAudio: Wasm AudioWorklet initialization failed.";
    return false;
  }
  if ( !ready_ ) {
    errorText = "RtApiWebAudio: await Module.RtAudioWeb.initialize({ context }) before openStream().";
    return false;
  }

  handles_.push_back( handle );
  if ( !createNode( handle, errorText ) ) {
    handles_.pop_back();
    return false;
  }
  return true;
}

void WebAudioRuntime::detach( WebAudioHandle *handle )
{
  if ( !handle ) return;

  const auto it = std::find( handles_.begin(), handles_.end(), handle );
  if ( it != handles_.end() ) handles_.erase( it );

  if ( handle->node > 0 ) {
    rtaudio_web_unpublish_node( handle->node, handle->name.c_str() );
    emscripten_destroy_web_audio_node( handle->node );
    handle->node = 0;
  }

  // owner is cleared before detach(). Wait only for callbacks which might have
  // loaded the old owner before that store. New callbacks are safe: they see a
  // null owner and never access the stream/user buffer.
  while ( handle->activeCallbacks.load( std::memory_order_acquire ) != 0 ) {}

  // Do not free callback userData: Emscripten currently has no synchronous
  // guarantee that the destroyed node cannot invoke process() again later.
  retiredHandles_.emplace_back( handle );
}

void WebAudioRuntime::workletThreadStarted( EMSCRIPTEN_WEBAUDIO_T context,
                                            bool success, void *userData )
{
  auto *runtime = static_cast<WebAudioRuntime *>( userData );
  if ( !runtime ) return;

  if ( !success ) {
    runtime->fail( "RtApiWebAudio: failed to create the Wasm AudioWorklet thread." );
    return;
  }

  WebAudioWorkletProcessorCreateOptions options{};
  options.name = kProcessorName;
  options.numAudioParams = 0;
  options.audioParamDescriptors = nullptr;

  emscripten_create_wasm_audio_worklet_processor_async(
    context, &options, &WebAudioRuntime::processorCreated, runtime );
}

void WebAudioRuntime::processorCreated( EMSCRIPTEN_WEBAUDIO_T /*context*/,
                                        bool success, void *userData )
{
  auto *runtime = static_cast<WebAudioRuntime *>( userData );
  if ( !runtime ) return;

  if ( !success ) {
    runtime->fail( "RtApiWebAudio: failed to create the Wasm AudioWorklet processor." );
    return;
  }

  runtime->initializing_ = false;
  runtime->ready_ = true;
  rtaudio_web_complete_initialization( 1, nullptr );
}

bool WebAudioRuntime::createNode( WebAudioHandle *handle,
                                  std::string &errorText )
{
  if ( !handle || handle->node > 0 ) return true;

  int outputChannelCount = static_cast<int>( handle->channels );
  EmscriptenAudioWorkletNodeCreateOptions options{};
  options.numberOfInputs = 0;
  options.numberOfOutputs = 1;
  options.outputChannelCounts = &outputChannelCount;
  options.channelCount = handle->channels;
  options.channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_EXPLICIT;
  options.channelInterpretation = WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS;

  handle->node = emscripten_create_wasm_audio_worklet_node(
    context_, kProcessorName, &options, &WebAudioRuntime::process, handle );
  if ( handle->node <= 0 ) {
    handle->node = 0;
    errorText = "RtApiWebAudio: failed to create the AudioWorkletNode.";
    return false;
  }

  // Standalone streams preserve the default RtAudio behavior. External graph
  // streams remain disconnected until browser JavaScript wires them up.
  if ( !handle->externalGraph )
    emscripten_audio_node_connect( handle->node, context_, 0, 0 );

  // Publish only after all native setup has completed. User callbacks are
  // deferred by rtaudio_web_publish_node(), so this call cannot synchronously
  // close the stream and invalidate handle while createNode() is unwinding.
  rtaudio_web_publish_node( handle->node, context_, handle->name.c_str(),
                            handle->externalGraph ? 1 : 0 );

  return true;
}

bool WebAudioRuntime::process( int /*numInputs*/, const AudioSampleFrame * /*inputs*/,
                               int numOutputs, AudioSampleFrame *outputs,
                               int /*numParams*/, const AudioParamFrame * /*params*/,
                               void *userData )
{
  auto *handle = static_cast<WebAudioHandle *>( userData );
  if ( !handle ) return false;

  // Increment before inspecting owner/running. This makes stopStream()'s
  // running=false + activeCallbacks==0 handshake safe: a process callback that
  // starts after stop sees running=false, while one that can still enter the
  // user callback is visible to the main thread as active.
  handle->activeCallbacks.fetch_add( 1, std::memory_order_acq_rel );

  if ( outputs ) {
    for ( int outputIndex = 0; outputIndex < numOutputs; ++outputIndex ) {
      AudioSampleFrame &output = outputs[outputIndex];
      if ( output.data && output.numberOfChannels > 0 && output.samplesPerChannel > 0 ) {
        const std::size_t count =
          static_cast<std::size_t>( output.numberOfChannels ) *
          static_cast<std::size_t>( output.samplesPerChannel );
        std::fill_n( output.data, count, 0.0f );
      }
    }
  }

  RtApiWebAudio *owner = handle->owner.load( std::memory_order_acquire );
  if ( owner && owner->running_.load( std::memory_order_acquire ) &&
       outputs && numOutputs > 0 ) {
    owner->callbackEvent( outputs[0] );
  }

  // Returning false asks the AudioWorklet processor to stop permanently once
  // a stream has detached. The handle is still retained as a safety net for
  // Emscripten versions where node destruction can race one final callback.
  const bool keepAlive = owner != nullptr;
  handle->activeCallbacks.fetch_sub( 1, std::memory_order_acq_rel );
  return keepAlive;
}

void WebAudioRuntime::synchronizeStoppedStreamsOnMainThread()
{
  WebAudioRuntime &runtime = instance();
  for ( WebAudioHandle *handle : runtime.handles_ ) {
    if ( !handle ) continue;
    RtApiWebAudio *owner = handle->owner.load( std::memory_order_acquire );
    if ( owner ) owner->synchronizeStoppedStateOnMainThread();
  }
}

void WebAudioRuntime::fail( const char *message )
{
  failed_ = true;
  initializing_ = false;
  ready_ = false;
  rtaudio_web_complete_initialization( 0, message );

  std::size_t index = 0;
  while ( index < handles_.size() ) {
    WebAudioHandle *handle = handles_[index];
    RtApiWebAudio *owner = handle->owner.load( std::memory_order_acquire );
    if ( owner ) owner->reportAsyncFailure( message );

    // reportAsyncFailure() invokes user code, which may close this or another
    // stream. Only advance if the current handle still occupies this slot.
    if ( index < handles_.size() && handles_[index] == handle ) ++index;
  }
}

void RtApiWebAudio::probeDevices()
{
  WebAudioRuntime &runtime = WebAudioRuntime::instance();

  if ( deviceList_.empty() ) {
    RtAudio::DeviceInfo info;
    info.ID = kWebAudioDeviceId;
    info.name = "WebAudio Default Output";
    info.outputChannels = kMaxWebAudioChannels;
    info.inputChannels = 0;
    info.duplexChannels = 0;
    info.isDefaultOutput = true;
    info.isDefaultInput = false;
    info.sampleRates = { 8000, 11025, 16000, 22050, 32000,
                         44100, 48000, 88200, 96000 };
    info.currentSampleRate = runtime.sampleRate();
    info.preferredSampleRate = runtime.sampleRate() ? runtime.sampleRate() : 48000;
    // WebAudio render buffers are natively planar float32. Other RtAudio user
    // formats remain supported through the explicit conversion in callbackEvent().
    info.nativeFormats = RTAUDIO_FLOAT32;
    deviceList_.push_back( info );
  }
  else {
    deviceList_[0].currentSampleRate = runtime.sampleRate();
    deviceList_[0].preferredSampleRate = runtime.sampleRate() ? runtime.sampleRate() : 48000;
  }
}

bool RtApiWebAudio::probeDeviceOpen( unsigned int deviceId, StreamMode mode,
                                     unsigned int channels,
                                     unsigned int firstChannel,
                                     unsigned int sampleRate,
                                     RtAudioFormat format,
                                     unsigned int *bufferSize,
                                     RtAudio::StreamOptions *options )
{
  if ( !emscripten_is_main_browser_thread() ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: openStream() must be called on the browser main thread.";
    return FAILURE;
  }

  if ( mode != OUTPUT ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: browser input is not yet supported; WebAudio output only.";
    if ( stream_.apiHandle ) closeStream();
    return FAILURE;
  }

  if ( deviceId != kWebAudioDeviceId ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: invalid WebAudio device ID.";
    return FAILURE;
  }

  if ( channels == 0 || channels > kMaxWebAudioChannels || firstChannel != 0 ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: WebAudio currently supports one or two output channels starting at channel zero.";
    return FAILURE;
  }

  if ( !bufferSize ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: bufferSize is null.";
    return FAILURE;
  }

  if ( sampleRate != 0 && ( sampleRate < 8000 || sampleRate > 96000 ) ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: WebAudio sample rate must be between 8000 and 96000 Hz.";
    return FAILURE;
  }

  WebAudioRuntime &runtime = WebAudioRuntime::instance();
  if ( !runtime.ready() ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: WebAudio runtime is not ready; await Module.RtAudioWeb.initialize({ context }) before openStream().";
    return FAILURE;
  }

  std::unique_ptr<WebAudioHandle> handle( new WebAudioHandle );
  handle->owner.store( this, std::memory_order_release );
  handle->channels = channels;
  handle->externalGraph = options &&
    ( options->flags & RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH );
  if ( options ) handle->name = options->streamName;

  const std::size_t bytesPerSample = formatBytes( format );
  const std::size_t userBufferBytes =
    static_cast<std::size_t>( runtime.quantumSize() ) * channels * bytesPerSample;
  char *userBuffer = static_cast<char *>( std::calloc( userBufferBytes, 1 ) );
  if ( !userBuffer ) {
    errorText_ = "RtApiWebAudio::probeDeviceOpen: failed to allocate the user audio buffer.";
    return FAILURE;
  }

  stream_.apiHandle = handle.get();
  stream_.mode = OUTPUT;
  stream_.state = STREAM_STOPPED;
  stream_.deviceId[0] = deviceId;
  stream_.sampleRate = runtime.sampleRate();
  stream_.bufferSize = runtime.quantumSize();
  stream_.nBuffers = 1;
  stream_.nUserChannels[0] = channels;
  stream_.nDeviceChannels[0] = channels;
  stream_.channelOffset[0] = 0;
  // Emscripten does not expose AudioContext.baseLatency/outputLatency through
  // this C API, so do not report the render quantum as device latency.
  stream_.latency[0] = 0;
  stream_.userFormat = format;
  stream_.deviceFormat[0] = RTAUDIO_FLOAT32;
  stream_.userInterleaved = !( options && ( options->flags & RTAUDIO_NONINTERLEAVED ) );
  stream_.deviceInterleaved[0] = false;
  // Conversion into the device's planar float32 buffers is performed manually
  // in callbackEvent(); the generic RtApi convertBuffer() path is not used.
  stream_.doConvertBuffer[0] = false;
  stream_.doByteSwap[0] = false;
  stream_.userBuffer[0] = userBuffer;
  stream_.callbackInfo.object = this;
  streamFrames_.store( 0, std::memory_order_release );
  *bufferSize = stream_.bufferSize;

  std::string runtimeError;
  if ( !runtime.attach( handle.get(), runtimeError ) ) {
    std::free( stream_.userBuffer[0] );
    stream_.userBuffer[0] = nullptr;
    stream_.apiHandle = nullptr;
    clearStreamInfo();
    errorText_ = runtimeError;
    return FAILURE;
  }

  handle.release();
  probeDevices();
  return SUCCESS;
}

void RtApiWebAudio::callbackEvent( AudioSampleFrame &output )
{
  if ( !stream_.callbackInfo.callback || !stream_.userBuffer[0] ) return;

  const unsigned int frames = static_cast<unsigned int>( output.samplesPerChannel );
  const unsigned int channels = std::min(
    stream_.nUserChannels[0], static_cast<unsigned int>( output.numberOfChannels ) );

  if ( frames == 0 || frames > stream_.bufferSize || channels == 0 ) return;

  const std::size_t bytes = static_cast<std::size_t>( stream_.bufferSize ) *
                            stream_.nUserChannels[0] * formatBytes( stream_.userFormat );
  std::memset( stream_.userBuffer[0], 0, bytes );

  const std::uint64_t startFrame =
    streamFrames_.load( std::memory_order_relaxed );
  const double callbackTime = stream_.sampleRate > 0
    ? static_cast<double>( startFrame ) / static_cast<double>( stream_.sampleRate )
    : 0.0;

  const int callbackResult = stream_.callbackInfo.callback(
    stream_.userBuffer[0], nullptr, frames, callbackTime,
    0, stream_.callbackInfo.userData );

  if ( callbackResult != 2 ) {
    for ( unsigned int channel = 0; channel < channels; ++channel ) {
      float *destination = output.data + channel * frames;
      for ( unsigned int frame = 0; frame < frames; ++frame )
        destination[frame] = sampleAsFloat( frame, channel );
    }
  }

  streamFrames_.fetch_add( frames, std::memory_order_relaxed );

  if ( callbackResult == 1 || callbackResult == 2 ) {
    running_.store( false, std::memory_order_release );
    // stream_.state is main-thread-owned. Emscripten explicitly provides this
    // low-frequency event path from AudioWorklet to main thread; use it only
    // for callback-requested stop/abort, never in the normal audio path.
    emscripten_audio_worklet_post_function_v(
      EMSCRIPTEN_AUDIO_MAIN_THREAD,
      &WebAudioRuntime::synchronizeStoppedStreamsOnMainThread );
  }
}

float RtApiWebAudio::sampleAsFloat( unsigned int frame,
                                    unsigned int channel ) const
{
  const unsigned int channels = stream_.nUserChannels[0];
  const unsigned int index = stream_.userInterleaved
    ? frame * channels + channel
    : channel * stream_.bufferSize + frame;

  const char *buffer = stream_.userBuffer[0];
  switch ( stream_.userFormat ) {
    case RTAUDIO_SINT8:
      return static_cast<float>( reinterpret_cast<const std::int8_t *>( buffer )[index] ) /
             128.0f;
    case RTAUDIO_SINT16:
      return static_cast<float>( reinterpret_cast<const std::int16_t *>( buffer )[index] ) /
             32768.0f;
    case RTAUDIO_SINT24: {
      // RtAudio stores SINT24 in its four-byte S24 container; the high byte is
      // padding and the low three bytes contain the signed 24-bit sample.
      const unsigned char *sample =
        reinterpret_cast<const unsigned char *>( buffer ) + index * 4;
      std::int32_t value = static_cast<std::int32_t>( sample[0] ) |
                           ( static_cast<std::int32_t>( sample[1] ) << 8 ) |
                           ( static_cast<std::int32_t>( sample[2] ) << 16 );
      if ( value & 0x00800000 ) value |= static_cast<std::int32_t>( 0xff000000 );
      return static_cast<float>( value ) / 8388608.0f;
    }
    case RTAUDIO_SINT32:
      return static_cast<float>(
        static_cast<double>( reinterpret_cast<const std::int32_t *>( buffer )[index] ) /
        2147483648.0 );
    case RTAUDIO_FLOAT32:
      return reinterpret_cast<const float *>( buffer )[index];
    case RTAUDIO_FLOAT64:
      return static_cast<float>( reinterpret_cast<const double *>( buffer )[index] );
    default:
      return 0.0f;
  }
}

RtAudioErrorType RtApiWebAudio::startStream()
{
  if ( !emscripten_is_main_browser_thread() ) {
    errorText_ = "RtApiWebAudio::startStream: startStream() must be called on the browser main thread.";
    return error( RTAUDIO_INVALID_USE );
  }

  if ( stream_.state == STREAM_CLOSED ) {
    errorText_ = "RtApiWebAudio::startStream: no open stream.";
    return error( RTAUDIO_WARNING );
  }

  WebAudioRuntime &runtime = WebAudioRuntime::instance();
  if ( initFailed_.load( std::memory_order_acquire ) || runtime.failed() ) {
    errorText_ = "RtApiWebAudio::startStream: AudioWorklet initialization failed.";
    return error( RTAUDIO_SYSTEM_ERROR );
  }

  if ( !runtime.ready() || runtime.context() <= 0 ) {
    errorText_ = "RtApiWebAudio::startStream: WebAudio runtime is not ready.";
    return error( RTAUDIO_SYSTEM_ERROR );
  }

  const AUDIO_CONTEXT_STATE before =
    emscripten_audio_context_state( runtime.context() );
  if ( running_.load( std::memory_order_acquire ) &&
       before == AUDIO_CONTEXT_STATE_RUNNING ) {
    errorText_ = "RtApiWebAudio::startStream: the stream is already running.";
    return error( RTAUDIO_WARNING );
  }

  // Browser autoplay rules require this call to originate from a user gesture.
  emscripten_resume_audio_context_sync( runtime.context() );
  if ( emscripten_audio_context_state( runtime.context() ) !=
       AUDIO_CONTEXT_STATE_RUNNING ) {
    running_.store( false, std::memory_order_release );
    stream_.state = STREAM_STOPPED;
    errorText_ = "RtApiWebAudio::startStream: AudioContext is still suspended; call startStream() from a browser user gesture (click/touch/key event).";
    return error( RTAUDIO_WARNING );
  }

  running_.store( true, std::memory_order_release );
  stream_.state = STREAM_RUNNING;
  return RTAUDIO_NO_ERROR;
}

RtAudioErrorType RtApiWebAudio::stopStream()
{
  if ( emscripten_current_thread_is_audio_worklet() ) {
    errorText_ = "RtApiWebAudio::stopStream: do not call stopStream() from the realtime callback; return 1 from the callback instead.";
    return error( RTAUDIO_INVALID_USE );
  }
  if ( !emscripten_is_main_browser_thread() ) {
    errorText_ = "RtApiWebAudio::stopStream: stopStream() must be called on the browser main thread.";
    return error( RTAUDIO_INVALID_USE );
  }
  if ( stream_.state == STREAM_CLOSED ) {
    errorText_ = "RtApiWebAudio::stopStream: no open stream.";
    return error( RTAUDIO_WARNING );
  }

  const bool wasRunning = running_.exchange( false, std::memory_order_acq_rel );
  if ( !wasRunning && stream_.state == STREAM_STOPPED ) {
    errorText_ = "RtApiWebAudio::stopStream: the stream is already stopped.";
    return error( RTAUDIO_WARNING );
  }

  // Once running=false is visible, a newly starting process quantum cannot
  // enter the user's callback. Drain a quantum that had already entered before
  // returning so callers may safely release callback-owned data after stop.
  waitForActiveCallback();

  // The AudioContext is shared by every RtAudio WebAudio stream, so stopping
  // one stream only silences its node; the shared context remains running.
  stream_.state = STREAM_STOPPED;
  return RTAUDIO_NO_ERROR;
}

RtAudioErrorType RtApiWebAudio::abortStream()
{
  if ( emscripten_current_thread_is_audio_worklet() ) {
    errorText_ = "RtApiWebAudio::abortStream: do not call abortStream() from the realtime callback; return 2 from the callback instead.";
    return error( RTAUDIO_INVALID_USE );
  }
  if ( !emscripten_is_main_browser_thread() ) {
    errorText_ = "RtApiWebAudio::abortStream: abortStream() must be called on the browser main thread.";
    return error( RTAUDIO_INVALID_USE );
  }
  if ( stream_.state == STREAM_CLOSED ) {
    errorText_ = "RtApiWebAudio::abortStream: no open stream.";
    return error( RTAUDIO_WARNING );
  }

  running_.store( false, std::memory_order_release );
  waitForActiveCallback();
  stream_.state = STREAM_STOPPED;
  return RTAUDIO_NO_ERROR;
}

void RtApiWebAudio::closeStream()
{
  if ( emscripten_current_thread_is_audio_worklet() ) {
    errorText_ = "RtApiWebAudio::closeStream: closeStream() cannot be called from the realtime callback; return 1 or 2 from the callback, then close on the main thread.";
    error( RTAUDIO_INVALID_USE );
    return;
  }

  if ( !emscripten_is_main_browser_thread() ) {
    errorText_ = "RtApiWebAudio::closeStream: closeStream() must be called on the browser main thread.";
    error( RTAUDIO_INVALID_USE );
    return;
  }

  if ( stream_.state == STREAM_CLOSED ) {
    errorText_ = "RtApiWebAudio::closeStream: no open stream.";
    error( RTAUDIO_WARNING );
    return;
  }

  running_.store( false, std::memory_order_release );
  initFailed_.store( false, std::memory_order_release );

  auto *handle = static_cast<WebAudioHandle *>( stream_.apiHandle );
  stream_.apiHandle = nullptr;

  if ( handle ) {
    // Publish the detached owner before destroying/disconnecting the node. Any
    // callback which starts later can only observe nullptr and therefore cannot
    // reach this RtApiWebAudio or its soon-to-be-freed user buffer.
    handle->owner.store( nullptr, std::memory_order_release );
    WebAudioRuntime::instance().detach( handle );
  }

  if ( stream_.userBuffer[0] ) {
    std::free( stream_.userBuffer[0] );
    stream_.userBuffer[0] = nullptr;
  }

  streamFrames_.store( 0, std::memory_order_release );
  clearStreamInfo();
}

double RtApiWebAudio::getStreamTime() const
{
  if ( stream_.state == STREAM_CLOSED || stream_.sampleRate == 0 ) return 0.0;
  return static_cast<double>( streamFrames_.load( std::memory_order_acquire ) ) /
         static_cast<double>( stream_.sampleRate );
}

void RtApiWebAudio::setStreamTime( double time )
{
  if ( time < 0.0 || stream_.sampleRate == 0 ) return;
  const double framePosition = time * static_cast<double>( stream_.sampleRate );
  streamFrames_.store(
    static_cast<std::uint64_t>( framePosition + 0.5 ),
    std::memory_order_release );
}

void RtApiWebAudio::reportAsyncFailure( const char *message )
{
  initFailed_.store( true, std::memory_order_release );
  running_.store( false, std::memory_order_release );
  stream_.state = STREAM_STOPPED;
  errorText_ = message;
  error( RTAUDIO_SYSTEM_ERROR );
}

bool initializeWebAudioRuntime( EMSCRIPTEN_WEBAUDIO_T context,
                                std::string &errorText )
{
  return WebAudioRuntime::instance().initialize( context, errorText );
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE
int rtaudio_web_initialize_context( EMSCRIPTEN_WEBAUDIO_T context )
{
  std::string errorText;
  if ( initializeWebAudioRuntime( context, errorText ) ) return 0;
  rtaudio_web_complete_initialization( 0, errorText.c_str() );
  return 1;
}

std::shared_ptr<RtApi> createRtApiWebAudio()
{
  return std::shared_ptr<RtApi>( new RtApiWebAudio() );
}

#endif // __RTAUDIO_WEB_AUDIO__
