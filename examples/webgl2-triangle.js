console.log("=== Mystral ANGLE WebGL2 Test ===");

const gl = canvas.getContext("webgl2", {
    alpha: false,
    depth: true,
    stencil: false,
    antialias: false,
    powerPreference: "high-performance"
});

if (!gl) {
    throw new Error("ANGLE WebGL2 context creation failed");
}

function compileShader(type, source) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(gl.getShaderInfoLog(shader));
    }
    return shader;
}

const vertexShader = compileShader(gl.VERTEX_SHADER, `#version 300 es
in vec2 position;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
}`);

const fragmentShader = compileShader(gl.FRAGMENT_SHADER, `#version 300 es
precision highp float;
out vec4 color;
void main() {
    color = vec4(0.15, 0.65, 1.0, 1.0);
}`);

const program = gl.createProgram();
gl.attachShader(program, vertexShader);
gl.attachShader(program, fragmentShader);
gl.linkProgram(program);
if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(program));
}

const vertices = new Float32Array([
     0.0,  0.75,
    -0.75, -0.75,
     0.75, -0.75
]);
const vertexBuffer = gl.createBuffer();
gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

const position = gl.getAttribLocation(program, "position");
gl.enableVertexAttribArray(position);
gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0);

gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
gl.clearColor(0.04, 0.06, 0.1, 1.0);
gl.clear(gl.COLOR_BUFFER_BIT);
gl.useProgram(program);
gl.drawArrays(gl.TRIANGLES, 0, 3);
gl.finish();

const center = new Uint8Array(4);
gl.readPixels(
    Math.floor(gl.drawingBufferWidth / 2),
    Math.floor(gl.drawingBufferHeight / 2),
    1,
    1,
    gl.RGBA,
    gl.UNSIGNED_BYTE,
    center
);

const passed = center[2] > center[0] && center[2] > center[1] && gl.getError() === gl.NO_ERROR;
console.log(`ANGLE renderer: ${gl.getParameter(gl.RENDERER)}`);
console.log(`WebGL version: ${gl.getParameter(gl.VERSION)}`);
console.log(`Center pixel: ${Array.from(center).join(",")}`);
console.log(`WEBGL2_TRIANGLE_RESULT=${passed ? "pass" : "fail"}`);
if (!passed) {
    throw new Error("ANGLE WebGL2 triangle validation failed");
}

let presentedFrames = 0;
function render() {
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    presentedFrames++;
    requestAnimationFrame(render);
}
requestAnimationFrame(render);
setTimeout(() => {
    console.log(`WEBGL2_PRESENTED_FRAMES=${presentedFrames}`);
    process.exit(0);
}, 1500);
