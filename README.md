# RtAudio

![Build Status](https://github.com/thestk/rtaudio/actions/workflows/ci.yml/badge.svg)

A set of C++ classes that provide a common API for realtime audio input/output across Linux (native ALSA, JACK, PulseAudio and OSS), Macintosh OS X (CoreAudio and JACK), Windows (DirectSound, ASIO and WASAPI), and Emscripten/WebAudio for browser output.

By Gary P. Scavone, 2001-2023 (and many other developers!)

This distribution of RtAudio contains the following:

- doc:      RtAudio documentation (see doc/html/index.html)
- tests:    example RtAudio programs, including WebAudio browser examples
- include:  header and source files necessary for ASIO, DS & OSS compilation
- tests/Windows: Visual C++ .net test program workspace and projects

## Overview

RtAudio is a set of C++ classes that provides a common API (Application Programming Interface) for realtime audio input/output across Linux (native ALSA, JACK, PulseAudio and OSS), Macintosh OS X, Windows (DirectSound, ASIO and WASAPI), and Emscripten/WebAudio. RtAudio significantly simplifies the process of interacting with computer audio hardware and browser audio output. It was designed with the following objectives:

  - object-oriented C++ design
  - simple, common API across all supported platforms
  - preserve straightforward source integration for native platforms
  - allow simultaneous multi-api support
  - support dynamic connection of devices
  - provide extensive audio device parameter control
  - allow audio device capability probing
  - automatic internal conversion for data format, channel number compensation, (de)interleaving, and byte-swapping where supported

RtAudio incorporates the concept of audio streams, which represent audio output (playback) and/or input (recording). Available audio devices and their capabilities can be enumerated and then specified when opening a stream. Where applicable, multiple API support can be compiled and a particular API specified when creating an RtAudio instance. See the \ref apinotes section for information specific to each of the supported audio APIs.

RtAudio is also offered as a module, which is enabled with `RTAUDIO_BUILD_MODULES`, and is accessed with `import rt.audio;`. Namespaces are implicitly imported (unless disabled with `RTAUDIO_USE_NAMESPACE`), so classes can be accessed through namespace `rt::audio` or through the global namespace (for example, `rt::audio::RtApi` and `::RtApi` are both valid).

## Building

Several build systems are available. These are:

  - autotools (`./autogen.sh; make` from git, or `./configure; make` from tarball release)
  - CMake (`mkdir build; cd build; ../cmake; make`)
  - meson (`meson build; cd build; ninja`)
  - vcpkg (`./bootstrap-vcpkg.sh; ./vcpkg integrate install; ./vcpkg install rtaudio`)

See `install.txt` for more instructions about how to select the audio backend API. By
default all detected APIs will be enabled.

For native backends, RtAudio retains the traditional direct-integration model around
`RtAudio.cpp` and `RtAudio.h`. The Emscripten backend additionally uses
`RtAudioWasm.cpp`, `RtAudioWeb.cpp` and `RtAudioWeb.h`, plus WebAudio/Wasm Worker link
flags. Use the CMake target for WebAudio builds so those requirements propagate to the
final application correctly.

## WebAssembly / WebAudio

CMake builds using the Emscripten toolchain enable the WebAudio backend by default. The
backend executes the RtAudio callback on a Wasm `AudioWorklet`, exposes one virtual
mono/stereo output device, and can optionally expose a RtAudio stream as a real browser
`AudioWorkletNode` for integration into a larger WebAudio graph.

WebAudio bootstrap is explicitly asynchronous. The web application creates the
`AudioContext`, initializes RtAudio with it, and only then opens synchronous RtAudio
streams:

```js
const context = new AudioContext({ latencyHint: 'interactive' });
await Module.RtAudioWeb.initialize({ context });
```

On the C++ side WebAudio is a first-class Emscripten-only API value:

```cpp
RtAudio audio(RtAudio::WEB_AUDIO);
```

Build all interactive browser examples with:

```sh
emcmake cmake -S . -B build-wasm -G Ninja \
  -DRTAUDIO_API_WEB_AUDIO=ON \
  -DRTAUDIO_BUILD_TESTING=ON
cmake --build build-wasm --target webaudio_examples
python3 tests/webaudio_server.py build-wasm/tests
```

`webaudio_examples` is the same aggregate target built by WebAudio CI. The CI also runs
the generated bundle in real headless Google Chrome and validates host AudioContext
adoption, AudioWorkletNode creation, controls, metering and Stop/Start node reuse.

Then open `http://127.0.0.1:8000/webaudio_playsaw.html`.

The current backend is output-only. Browser input/duplex requires the asynchronous
`getUserMedia()` permission flow and is planned as a direct
`MediaStreamAudioSourceNode -> RtAudio AudioWorkletNode` connection.

See [WEBAUDIO.md](WEBAUDIO.md) for bootstrap, build flags, cross-origin isolation,
realtime/lifecycle guarantees, external graph integration, stream time, teardown and
browser-test details.

## FAQ

### Why does audio only come to one ear when I choose 1-channel output?

RtAudio doesn't automatically turn 1-channel output into stereo output with copied values to two channels, since there may be cases when a user truly wants 1-channel behaviour. If you want monophonic data to be projected to stereo output, open a 2-channel stream and copy the data to both channels in your audio stream callback.

## Further Reading

For complete documentation on RtAudio, see the doc directory of the distribution or surf to https://caml.music.mcgill.ca/~gary/rtaudio/index.html.


## Legal and ethical:

The RtAudio license is similar to the MIT License. Please see [LICENSE](LICENSE).
