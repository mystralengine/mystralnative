// Memory leak test: RAF loop with JS object allocations (no WebGPU)
// If this leaks but bare-raf doesn't, the issue is V8 GC not collecting JS objects
console.log("memleak-raf-alloc: RAF loop with JS allocations");

let frameCount = 0;
function tick(timestamp) {
    // Create some throwaway objects each frame (similar to what WebGPU code does)
    const descriptor = {
        colorAttachments: [{
            view: null,
            loadOp: "clear",
            storeOp: "store",
            clearValue: { r: 0.1, g: 0.1, b: 0.1, a: 1.0 }
        }]
    };
    const arr = [1, 2, 3, 4, 5, 6, 7, 8];
    const str = "frame-" + frameCount;
    const nested = { a: { b: { c: descriptor } } };

    frameCount++;
    if (frameCount % 600 === 0) {
        console.log("Frame:", frameCount);
    }
    requestAnimationFrame(tick);
}
requestAnimationFrame(tick);
