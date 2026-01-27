#!/usr/bin/env node
/**
 * Updates the MystralNative version in all source files.
 *
 * Usage: node scripts/set-version.mjs <version>
 *   e.g. node scripts/set-version.mjs 0.0.8
 *
 * Updates:
 *   - package.json          ("version": "x.y.z")
 *   - CMakeLists.txt        (project VERSION x.y.z)
 *   - include/mystral/runtime.h  (#define MYSTRAL_VERSION "x.y.z")
 */

import { readFileSync, writeFileSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const root = join(__dirname, '..');

const version = process.argv[2];
if (!version || !/^\d+\.\d+\.\d+/.test(version)) {
  console.error('Usage: node scripts/set-version.mjs <version>');
  console.error('  e.g. node scripts/set-version.mjs 0.0.8');
  process.exit(1);
}

let updated = 0;

// 1. package.json
const pkgPath = join(root, 'package.json');
const pkg = JSON.parse(readFileSync(pkgPath, 'utf-8'));
if (pkg.version !== version) {
  pkg.version = version;
  writeFileSync(pkgPath, JSON.stringify(pkg, null, 2) + '\n');
  console.log(`  package.json: ${pkg.version} -> ${version}`);
  updated++;
} else {
  console.log(`  package.json: already ${version}`);
}

// 2. CMakeLists.txt
const cmakePath = join(root, 'CMakeLists.txt');
let cmake = readFileSync(cmakePath, 'utf-8');
const cmakeRe = /(project\(MystralNativeRuntime\s+VERSION\s+)\S+/;
const cmakeMatch = cmake.match(cmakeRe);
if (cmakeMatch) {
  const newCmake = cmake.replace(cmakeRe, `$1${version}`);
  if (newCmake !== cmake) {
    writeFileSync(cmakePath, newCmake);
    console.log(`  CMakeLists.txt: updated to ${version}`);
    updated++;
  } else {
    console.log(`  CMakeLists.txt: already ${version}`);
  }
} else {
  console.error('  CMakeLists.txt: WARNING - could not find project VERSION line');
}

// 3. include/mystral/runtime.h
const runtimePath = join(root, 'include', 'mystral', 'runtime.h');
let runtime = readFileSync(runtimePath, 'utf-8');
const runtimeRe = /(#define\s+MYSTRAL_VERSION\s+")([^"]+)(")/;
const runtimeMatch = runtime.match(runtimeRe);
if (runtimeMatch) {
  const newRuntime = runtime.replace(runtimeRe, `$1${version}$3`);
  if (newRuntime !== runtime) {
    writeFileSync(runtimePath, newRuntime);
    console.log(`  runtime.h: ${runtimeMatch[2]} -> ${version}`);
    updated++;
  } else {
    console.log(`  runtime.h: already ${version}`);
  }
} else {
  console.error('  runtime.h: WARNING - could not find MYSTRAL_VERSION define');
}

console.log(`\nVersion set to ${version} (${updated} file(s) updated)`);
