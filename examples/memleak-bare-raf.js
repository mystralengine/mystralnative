// Memory leak test: Bare RAF loop - no WebGPU, no allocations
// If this leaks, the issue is in the RAF/engine infrastructure itself
console.log("memleak-bare-raf: starting bare RAF loop");

let frameCount = 0;
function tick(timestamp) {
    frameCount++;
    if (frameCount % 600 === 0) {
        console.log("Frame:", frameCount);
    }
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
