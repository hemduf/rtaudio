// Browser-side graph wiring for the RtAudio external-node playsaw demo.
// RtAudio publishes the real AudioWorkletNode under Module.RtAudioWeb.

Module.RtAudioWeb = Module.RtAudioWeb || {};
Module.RtAudioWeb.onNodeCreated = function(info) {
  if (!info || info.name !== 'rtaudio-playsaw') return;

  const context = info.context;
  const node = info.node;

  // Demonstrate that the RtAudio application is a normal WebAudio node by
  // inserting a native GainNode between it and the destination.
  const gain = context.createGain();
  gain.gain.value = 1.0;
  node.connect(gain).connect(context.destination);

  Module.RtAudioWeb.playsawGraph = {
    source: node,
    gain: gain,
    destination: context.destination
  };

  console.log('RtAudio WebAudio node connected:', info.name, node);
};

Module.RtAudioWeb.onNodeDestroyed = function(info) {
  if (!info || info.name !== 'rtaudio-playsaw') return;

  const graph = Module.RtAudioWeb.playsawGraph;
  if (!graph || graph.source !== info.node) return;

  // Stop/Start intentionally keeps this graph alive. This callback runs only
  // when the RtAudio stream is actually closed/destroyed, so clean up the
  // browser-owned GainNode at the same lifecycle boundary.
  try {
    graph.gain.disconnect();
  } catch (error) {
    console.warn('Unable to disconnect playsaw GainNode:', error);
  }

  delete Module.RtAudioWeb.playsawGraph;
};
