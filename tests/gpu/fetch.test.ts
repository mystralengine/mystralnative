/**
 * Fetch API Tests
 *
 * Tests fetch() with file:// and http:// - no GPU required (runs headless with early exit).
 * These tests run in CI.
 */

import { describe, it, expect, beforeAll } from "bun:test";
import { spawn } from "bun";
import { existsSync, writeFileSync, mkdirSync, rmSync } from "fs";
import { join } from "path";

const MYSTRAL_BIN = join(import.meta.dir, "../../build/mystral");
const TEST_DIR = join(import.meta.dir, "../../.test-tmp");

describe("Fetch API", () => {
  beforeAll(() => {
    // Create test directory
    if (!existsSync(TEST_DIR)) {
      mkdirSync(TEST_DIR, { recursive: true });
    }

    // Create test JSON file
    writeFileSync(
      join(TEST_DIR, "test.json"),
      JSON.stringify({ message: "Hello from test", value: 42 })
    );

    // Create test text file
    writeFileSync(join(TEST_DIR, "test.txt"), "Hello, World!");
  });

  it("should fetch local JSON file", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    // Create a test script that fetches the JSON file
    const testScript = `
      async function main() {
        try {
          const response = await fetch('file://${join(TEST_DIR, "test.json")}');
          if (!response.ok) {
            console.log('FAIL: response not ok');
            return;
          }
          const data = await response.json();
          if (data.message === 'Hello from test' && data.value === 42) {
            console.log('PASS: JSON fetch works');
          } else {
            console.log('FAIL: unexpected data', JSON.stringify(data));
          }
        } catch (e) {
          console.log('FAIL: ' + e.message);
        }
      }
      main();
    `;

    writeFileSync(join(TEST_DIR, "fetch-test.js"), testScript);

    const screenshotPath = join(TEST_DIR, "fetch-test-screenshot.png");
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        join(TEST_DIR, "fetch-test.js"),
        "--headless",
        "--screenshot",
        screenshotPath,
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("PASS: JSON fetch works");
  });

  it("should fetch local text file", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const testScript = `
      async function main() {
        try {
          const response = await fetch('file://${join(TEST_DIR, "test.txt")}');
          if (!response.ok) {
            console.log('FAIL: response not ok');
            return;
          }
          const text = await response.text();
          if (text === 'Hello, World!') {
            console.log('PASS: text fetch works');
          } else {
            console.log('FAIL: unexpected text: ' + text);
          }
        } catch (e) {
          console.log('FAIL: ' + e.message);
        }
      }
      main();
    `;

    writeFileSync(join(TEST_DIR, "fetch-text-test.js"), testScript);

    const screenshotPath = join(TEST_DIR, "fetch-text-screenshot.png");
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        join(TEST_DIR, "fetch-text-test.js"),
        "--headless",
        "--screenshot",
        screenshotPath,
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("PASS: text fetch works");
  });

  it("should return 404 for nonexistent file", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const testScript = `
      async function main() {
        try {
          const response = await fetch('file://${join(TEST_DIR, "nonexistent.txt")}');
          if (response.status === 404 && !response.ok) {
            console.log('PASS: 404 for nonexistent file');
          } else {
            console.log('FAIL: expected 404, got ' + response.status);
          }
        } catch (e) {
          console.log('FAIL: ' + e.message);
        }
      }
      main();
    `;

    writeFileSync(join(TEST_DIR, "fetch-404-test.js"), testScript);

    const screenshotPath = join(TEST_DIR, "fetch-404-screenshot.png");
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        join(TEST_DIR, "fetch-404-test.js"),
        "--headless",
        "--screenshot",
        screenshotPath,
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("PASS: 404 for nonexistent file");
  });

  it("should support arrayBuffer()", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const testScript = `
      async function main() {
        try {
          const response = await fetch('file://${join(TEST_DIR, "test.txt")}');
          const buffer = await response.arrayBuffer();
          if (buffer instanceof ArrayBuffer && buffer.byteLength === 13) {
            console.log('PASS: arrayBuffer works');
          } else {
            console.log('FAIL: unexpected buffer size ' + buffer.byteLength);
          }
        } catch (e) {
          console.log('FAIL: ' + e.message);
        }
      }
      main();
    `;

    writeFileSync(join(TEST_DIR, "fetch-buffer-test.js"), testScript);

    const screenshotPath = join(TEST_DIR, "fetch-buffer-screenshot.png");
    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        join(TEST_DIR, "fetch-buffer-test.js"),
        "--headless",
        "--screenshot",
        screenshotPath,
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("PASS: arrayBuffer works");
  });

  it("should support XMLHttpRequest text responses and events", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const testScript = `
      const states = [];
      const xhr = new XMLHttpRequest();
      xhr.onreadystatechange = () => states.push(xhr.readyState);
      xhr.open('GET', 'file://${join(TEST_DIR, "test.txt")}');
      xhr.setRequestHeader('X-Test', 'one');
      xhr.setRequestHeader('X-Test', 'two');
      xhr.onprogress = (event) => {
        if (event.loaded !== 13 || event.total !== 13 || !event.lengthComputable) {
          console.log('FAIL: invalid progress event');
        }
      };
      xhr.onload = () => {
        const passed = xhr.status === 200 && xhr.responseText === 'Hello, World!' &&
          states.includes(XMLHttpRequest.HEADERS_RECEIVED) &&
          states[states.length - 1] === XMLHttpRequest.DONE;
        console.log(passed ? 'PASS: XMLHttpRequest works' : 'FAIL: invalid XMLHttpRequest result');
      };
      xhr.onerror = () => console.log('FAIL: XMLHttpRequest error');
      xhr.send();
    `;

    writeFileSync(join(TEST_DIR, "xhr-test.js"), testScript);

    const proc = spawn({
      cmd: [
        MYSTRAL_BIN,
        "run",
        join(TEST_DIR, "xhr-test.js"),
        "--headless",
        "--screenshot",
        join(TEST_DIR, "xhr-test-screenshot.png"),
        "--frames",
        "10",
      ],
      stdout: "pipe",
      stderr: "pipe",
    });

    const stdout = await new Response(proc.stdout).text();
    await proc.exited;

    expect(stdout).toContain("PASS: XMLHttpRequest works");
  });

  it("should forward XMLHttpRequest methods, headers, bodies, and response headers", async () => {
    if (!existsSync(MYSTRAL_BIN)) {
      console.log("Skipping: mystral binary not found");
      return;
    }

    const server = Bun.serve({
      hostname: "127.0.0.1",
      port: 0,
      async fetch(request) {
        const body = await request.text();
        return new Response(
          `${request.method}|${request.headers.get("x-test")}|${body}`,
          { headers: { "X-Reply": "seen" } }
        );
      },
    });

    try {
      const testScript = `
        const xhr = new XMLHttpRequest();
        xhr.open('PATCH', '${server.url}resource');
        xhr.setRequestHeader('X-Test', 'one');
        xhr.setRequestHeader('X-Test', 'two');
        xhr.onload = () => {
          const passed = xhr.status === 200 &&
            xhr.responseText === 'PATCH|one, two|payload' &&
            xhr.getResponseHeader('x-reply') === 'seen';
          console.log(passed ? 'PASS: XMLHttpRequest options work' :
            'FAIL: ' + xhr.status + '|' + xhr.responseText + '|' + xhr.getResponseHeader('x-reply'));
        };
        xhr.onerror = () => console.log('FAIL: XMLHttpRequest options error');
        xhr.send('payload');
      `;
      writeFileSync(join(TEST_DIR, "xhr-options-test.js"), testScript);

      const proc = spawn({
        cmd: [
          MYSTRAL_BIN,
          "run",
          join(TEST_DIR, "xhr-options-test.js"),
          "--headless",
          "--screenshot",
          join(TEST_DIR, "xhr-options-test-screenshot.png"),
          "--frames",
          "120",
        ],
        stdout: "pipe",
        stderr: "pipe",
      });

      const stdout = await new Response(proc.stdout).text();
      await proc.exited;
      expect(stdout).toContain("PASS: XMLHttpRequest options work");
    } finally {
      server.stop(true);
    }
  });
});
