# WebAssembly / WebAudio backend

RtAudio can be built with Emscripten and use a Wasm `AudioWorklet` as its
realtime output backend. The RtAudio callback executes on the browser audio
thread; JavaScript owns browser lifecycle and WebAudio graph integration.

## Quick start

Build all browser examples:

```sh
emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DRTAUDIO_API_WEB_AUDIO=ON \
  -DRTAUDIO_BUILD_TESTING=ON

cmake --build build-wasm --target webaudio_examples
```

`webaudio_examples` is the aggregate target also built by CI. Individual
examples can still be built directly:

```sh
cmake --build build-wasm --target webaudio_playsaw
```

Serve the generated bundle with the repository development server:

```sh
python3 tests/webaudio_server.py build-wasm/tests
```

Then open:

```text
http://127.0.0.1:8000/webaudio_playsaw.html
```

A plain `python -m http.server` is insufficient because Wasm Workers require a
cross-origin-isolated page.

## Browser bootstrap

WebAudio initialization is deliberately separate from `RtAudio::openStream()`.
Browser setup is asynchronous, while the public RtAudio stream API is
synchronous.

Create the `AudioContext` in the web application and initialize RtAudio before
opening any C++ stream:

```js
const context = new AudioContext({ latencyHint: 'interactive' });

await Module.RtAudioWeb.initialize({ context });
```

`initialize()` registers that JavaScript `AudioContext` with Emscripten, starts
the Wasm AudioWorklet thread, registers the RtAudio processor and resolves only
when the runtime is ready for synchronous stream creation.

The application-provided context becomes the context returned by:

```js
Module.RtAudioWeb.getContext();
```

Calling `openStream()` before initialization is complete is rejected. This
avoids the previous intermediate state where a RtAudio stream was reported as
open while its `AudioWorkletNode` was still being initialized asynchronously.

`initialize()` is idempotent for the same context. A RtAudio WASM module uses a
single AudioContext/AudioWorklet runtime for its page lifetime; attempting to
reinitialize it with a different context is rejected.

The API can create a context when none is supplied, but explicitly passing the
host application's context is recommended for integration into an existing
WebAudio graph.

## Selecting WebAudio from C++

WebAudio is a first-class `RtAudio::Api` value on Emscripten builds:

```cpp
#include "RtAudioWeb.h"

RtAudio audio(RtAudio::WEB_AUDIO);
```

The enum member exists only when compiling with Emscripten, immediately before
`NUM_APIS`. Native builds therefore keep their historical enum values and ABI.

Name-based discovery remains supported:

```cpp
const RtAudio::Api api = RtAudio::getCompiledApiByName("webaudio");
```

`RTAUDIO_WEB_AUDIO` in `RtAudioWeb.h` remains as a compatibility alias for
`RtAudio::WEB_AUDIO`.

The C API exposes `RTAUDIO_API_WEB_AUDIO` and
`RTAUDIO_FLAGS_WEB_AUDIO_EXTERNAL_GRAPH` on Emscripten builds.

## Build integration

`RTAUDIO_API_WEB_AUDIO` defaults to `ON` for the Emscripten toolchain and is
currently the only RtAudio backend available there. Configuring Emscripten with
`RTAUDIO_API_WEB_AUDIO=OFF` is rejected.

The CMake target propagates the required Emscripten options:

- compile: `-sWASM_WORKERS -pthread`
- link: `-sAUDIO_WORKLET -sWASM_WORKERS -pthread`
- compile definition: `__RTAUDIO_WEB_AUDIO__`

The backend's JavaScript bridge also declares its Emscripten library
dependencies (`emscriptenRegisterAudioObject` and `emscriptenGetAudioObject`)
from the WASM runtime object itself. The bridge therefore works for normal
in-tree builds, installed CMake targets and pkg-config consumers rather than
only for the repository examples.

The installed package contains `RtAudio.h`, `RtAudioWeb.h`, `rtaudio_c.h`, the
`RtAudio::rtaudio` CMake target and `rtaudio.pc`.

## Device model

The backend exposes one virtual WebAudio output device. WebAudio render buffers
are planar `float32`, so `DeviceInfo::nativeFormats` reports only
`RTAUDIO_FLOAT32`.

RtAudio callbacks may still request SINT8, SINT16, SINT24, SINT32, FLOAT32 or
FLOAT64. The backend converts those callback buffers into native planar
FLOAT32 output.

The browser context owns the actual sample rate. `getStreamSampleRate()` returns
that rate after a stream is opened. The WebAudio render quantum, normally 128
frames, is returned through `bufferFrames`.

## Cross-origin isolation

The current high-performance backend uses Wasm Workers and shared memory. The
page therefore needs `SharedArrayBuffer` and cross-origin isolation.

A typical server configuration sends:

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

The repository `webaudio_server.py` additionally sends a suitable resource
policy and disables caching for development.

## Using RtAudio as a WebAudio node

Add `RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH` when opening the stream:

```cpp
RtAudio::StreamOptions options;
options.flags = RTAUDIO_MINIMIZE_LATENCY |
                RTAUDIO_WEB_AUDIO_EXTERNAL_GRAPH;
options.streamName = "my-synth";
```

In this mode RtAudio creates its realtime `AudioWorkletNode` but does not
connect it automatically to `AudioContext.destination`.

The real node is published to JavaScript:

```js
Module.RtAudioWeb.onNodeCreated = ({ name, node, context }) => {
  if (name !== 'my-synth') return;

  const gain = context.createGain();
  const analyser = context.createAnalyser();

  node
    .connect(gain)
    .connect(analyser)
    .connect(context.destination);
};
```

The node registry is updated synchronously during `openStream()`. The user
notification is intentionally deferred to a JavaScript microtask so arbitrary
application JavaScript cannot re-enter RtAudio while native stream state is
being mutated.

After `openStream()` returns, the node can therefore also be queried directly:

```js
const node = Module.RtAudioWeb.getNode('my-synth');
const context = Module.RtAudioWeb.getContext();
```

`getNode()` also accepts the numeric Emscripten node handle.

Streams using the normal auto-connect mode are published as well. Closing a
stream removes it from the registry and schedules `onNodeDestroyed`.

## Realtime communication

The user callback executes on the Wasm AudioWorklet thread. The normal render
path follows realtime-audio rules:

- do not allocate or free memory in the callback;
- do not lock a mutex or wait on another thread;
- do not call browser JavaScript or the DOM;
- do not log through `std::cout`, `std::cerr` or `console`;
- use atomics or preallocated lock-free structures for controls and metering.

The playsaw example uses 32-bit atomics for frequent communication:

```text
frequency/gain UI
       |
       v
32-bit atomic targets
       |
       v
AudioWorklet callback
       |
       +----> atomic acknowledgements ----> UI
       |
       +----> atomic L/R peaks ------------> meters
```

Parameter targets are loaded once per render quantum and smoothed across the
block. There is no synchronization in the sample loop.

For rare lifecycle events, RtAudio uses
`emscripten_audio_worklet_post_function_*()` to synchronize state back to the
browser main thread. It is not used for the normal render path.

## Start, stop and callback lifetime

`startStream()`, `stopStream()`, `abortStream()` and `closeStream()` are
main-browser-thread operations.

Browsers may initially suspend the host `AudioContext`. Call `startStream()`
from a user gesture such as click, touch or key input. The backend resumes the
same host-owned context rather than creating a second context.

Explicit `stopStream()` and `abortStream()` first clear the realtime running
flag and then wait for a user callback that had already entered to finish. A
later AudioWorklet quantum renders silence without invoking user code.
Consequently, after the function returns, callback-owned application data can
safely be released or mutated.

Do not call stop/abort/close directly from the realtime callback. Use RtAudio's
normal callback return convention:

- `0`: continue;
- `1`: stop after the current block;
- `2`: abort and keep the current output quantum silent.

For callback return values 1 and 2, the audio thread clears the running flag and
posts one low-frequency main-thread state synchronization message.

## Stream time

The WebAudio backend tracks stream position using an atomic 64-bit frame counter.
Callback `streamTime`, `getStreamTime()` and `setStreamTime()` derive from that
frame position and the host AudioContext sample rate.

This avoids sharing RtAudio's internal `double streamTime` between the browser
main thread and AudioWorklet thread. `setStreamTime()` is quantized to the
nearest frame.

## Stream teardown

Current Emscripten node destruction has no synchronous completion fence proving
that a previously scheduled `process()` invocation cannot arrive after
`emscripten_destroy_web_audio_node()`.

RtAudio therefore clears the stream owner first, drains callbacks that had
already acquired that owner, frees the real RtAudio user buffer and retains only
the small callback handle for page lifetime. A late callback sees a null owner,
renders silence and returns `false`, terminating the processor without touching
freed stream state.

For normal pause/resume, prefer `stopStream()` followed by `startStream()`.
`webaudio_playsaw` follows this pattern and reuses the same AudioWorkletNode and
browser graph.

## Browser integration test

The WebAudio CI runs a real headless Google Chrome test through Playwright. It
serves the generated bundle with COOP/COEP headers and verifies the complete
browser path:

1. the page is cross-origin isolated;
2. the example creates a host-owned `AudioContext`;
3. `await Module.RtAudioWeb.initialize({ context })` becomes ready;
4. RtAudio adopts the exact same context;
5. Start creates and runs the real AudioWorkletNode;
6. the node is present in the external WebAudio graph;
7. frequency/gain changes reach the audio callback;
8. AudioWorklet peak meters return non-zero data to JavaScript;
9. Stop followed by Start reuses the same node.

The CI also builds the static library, CTest API smoke test, all WebAudio
examples, installed CMake consumer and installed pkg-config consumer.

## Installed consumers

CMake:

```cmake
find_package(RtAudio CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE RtAudio::rtaudio)
```

The project currently installs `RtAudioConfig.cmake` under `share/rtaudio`; set
`RtAudio_DIR` to that directory if the prefix is not in a CMake search location.

pkg-config:

```sh
em++ main.cpp $(pkg-config --cflags --libs rtaudio) -o my_app.js
```

Both installed-consumer paths are compiled and linked in WebAudio CI.

## Current scope

- Output: mono and stereo.
- Host-owned AudioContext with explicit asynchronous bootstrap.
- Native device format: planar FLOAT32.
- Callback formats: SINT8, SINT16, SINT24, SINT32, FLOAT32 and FLOAT64.
- Interleaved and `RTAUDIO_NONINTERLEAVED` callback buffers.
- Realtime callback on the Emscripten Wasm AudioWorklet thread.
- Optional exposure as a real browser `AudioWorkletNode`.
- Input/duplex is not implemented yet.

The intended input architecture is:

```text
getUserMedia()
     |
     v
MediaStreamAudioSourceNode
     |
     v
RtAudio AudioWorkletNode input
     |
     v
normal RtAudio inputBuffer callback
```

The asynchronous microphone permission flow will remain on the JavaScript side
before synchronous `openStream()`, while audio samples stay inside the WebAudio
and RtAudio callback path rather than being copied through JavaScript.
