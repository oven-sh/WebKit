// Reads the internals object when the script starts, before any events.
function internalsState()
{
    var state = {
        internals: typeof self.internals,
        settings: "",
        simulateEventForWebGLContext: "",
        webglMaxDrawingBufferSize: "",
        requestedGPU: "",
        webglMaxDrawingBufferSizeResult: "",
        requestedGPUResult: ""
    };
    if (!self.internals)
        return state;
    state.settings = typeof internals.settings;
    state.simulateEventForWebGLContext = typeof internals.simulateEventForWebGLContext;
    state.webglMaxDrawingBufferSize = typeof internals.webglMaxDrawingBufferSize;
    state.requestedGPU = typeof internals.requestedGPU;
    var gl = new OffscreenCanvas(1, 1).getContext("webgl");
    if (!gl)
        return state;
    var size = internals.webglMaxDrawingBufferSize(gl);
    state.webglMaxDrawingBufferSizeResult = size.length == 2 && size[0] > 0 && size[1] > 0 ? "valid" : "invalid: " + size;
    state.requestedGPUResult = internals.requestedGPU(gl);
    return state;
}

var state = internalsState();
if (self.SharedWorkerGlobalScope && self instanceof SharedWorkerGlobalScope)
    self.onconnect = function(event) { event.ports[0].postMessage(state); };
else
    self.postMessage(state);
