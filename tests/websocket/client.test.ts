import { describe, expect, it } from "bun:test";
import { spawn } from "bun";
import { existsSync, mkdirSync, writeFileSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");
const TEST_DIR = join(import.meta.dir, "../../.test-tmp");

describe("WebSocket API", () => {
  it("should exchange text and binary messages and close cleanly", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }
    mkdirSync(TEST_DIR, { recursive: true });

    const server = Bun.serve({
      hostname: "127.0.0.1",
      port: 0,
      fetch(request, server) {
        if (
          server.upgrade(request, {
            headers: { "Sec-WebSocket-Protocol": "echo-protocol" },
          })
        ) {
          return;
        }
        return new Response("WebSocket upgrade required", { status: 426 });
      },
      websocket: {
        message(socket, message) {
          socket.send(message);
        },
      },
    });

    try {
      const url = server.url.href.replace(/^http/, "ws");
      const script = `
        const received = [];
        const socket = new WebSocket('${url}', 'echo-protocol');
        socket.binaryType = 'arraybuffer';
        socket.onopen = () => {
          socket.send('native-text');
          socket.send(new Uint8Array([7, 11, 13, 17]));
        };
        socket.onmessage = event => {
          received.push(typeof event.data === 'string'
            ? 'text:' + event.data
            : 'binary:' + Array.from(new Uint8Array(event.data)).join(','));
          if (received.length === 2) socket.close(4001, 'complete');
        };
        socket.onerror = event => {
          console.log('FAIL: ' + (event.message || 'WebSocket error'));
          process.exit(1);
        };
        socket.onclose = event => {
          const passed = socket.protocol === 'echo-protocol' &&
            received.includes('text:native-text') &&
            received.includes('binary:7,11,13,17') &&
            event.code === 4001 && event.reason === 'complete' && event.wasClean;
          console.log(passed ? 'PASS: WebSocket works' : 'FAIL: invalid WebSocket result');
          process.exit(passed ? 0 : 1);
        };
        setTimeout(() => { console.log('FAIL: timeout'); process.exit(1); }, 10000);
      `;
      const scriptPath = join(TEST_DIR, "websocket-test.js");
      writeFileSync(scriptPath, script);

      const proc = spawn({
        cmd: [MYSTRAL_BIN, "run", scriptPath, "--headless"],
        stdout: "pipe",
        stderr: "pipe",
      });
      const stdout = await new Response(proc.stdout).text();
      const exitCode = await proc.exited;

      expect(exitCode).toBe(0);
      expect(stdout).toContain("PASS: WebSocket works");
    } finally {
      server.stop(true);
    }
  }, 30_000);
});
