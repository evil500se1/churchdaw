const fs = require("fs");
const path = require("path");

const requestedTarget = process.argv[2] || "temp";

if (requestedTarget === "claude-libpack-tgz") {
  const allowedRoot = path.resolve(
    "C:\\Users\\Administrator\\AppData\\Local\\Temp\\claude\\C--Users-Administrator\\0484deea-0307-46a3-a0f1-8a058074c0c3\\scratchpad\\libpack"
  );
  const allowedNames = ["pack1.tgz", "pack2.tgz", "pack3.tgz", "pack4.tgz"];
  let removedFiles = 0;
  let removedBytes = 0;

  for (const name of allowedNames) {
    const candidate = path.resolve(allowedRoot, name);
    if (
      path.dirname(candidate) !== allowedRoot ||
      path.basename(candidate) !== name
    ) {
      throw new Error(`unsafe Claude temporary: ${candidate}`);
    }

    if (!fs.existsSync(candidate)) continue;

    const stat = fs.lstatSync(candidate);
    if (!stat.isFile() || stat.isSymbolicLink()) {
      throw new Error(`unexpected Claude temporary type: ${candidate}`);
    }

    fs.unlinkSync(candidate);
    removedFiles += 1;
    removedBytes += stat.size;
  }

  process.stdout.write(
    `Removed ${removedFiles} stale Claude TGZ temporaries (${removedBytes} bytes)`
  );
  process.exit(0);
}

if (requestedTarget === "compiler-preprocessed") {
  const tempRoot = path.resolve(
    "C:\\Users\\Administrator\\AppData\\Local\\Temp"
  );
  const allowedName =
    /^(?:MainComponent|include_juce_[A-Za-z0-9_]+)-[0-9a-f]{6}\.cpp$/;
  const cutoffMs = Date.now() - 30 * 60 * 1000;
  let removedFiles = 0;
  let removedBytes = 0;

  for (const name of fs.readdirSync(tempRoot)) {
    if (!allowedName.test(name)) continue;

    const candidate = path.resolve(tempRoot, name);
    if (path.dirname(candidate) !== tempRoot) {
      throw new Error(`unsafe compiler temporary: ${candidate}`);
    }

    const stat = fs.lstatSync(candidate);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.mtimeMs > cutoffMs) {
      continue;
    }

    fs.unlinkSync(candidate);
    removedFiles += 1;
    removedBytes += stat.size;
  }

  process.stdout.write(
    `Removed ${removedFiles} compiler temporaries (${removedBytes} bytes)`
  );
  process.exit(0);
}

const targets = {
  temp: path.resolve(
    "C:\\Users\\Administrator\\AppData\\Local\\Temp\\ChurchDAW-x86-799dc56e69b2401480494dc365588875\\Builds"
  ),
  "project-cxx": path.resolve(
    "C:\\Users\\Administrator\\MinhaDAW\\Builds\\Android\\app\\.cxx"
  ),
  "project-app-build": path.resolve(
    "C:\\Users\\Administrator\\MinhaDAW\\Builds\\Android\\app\\build"
  ),
};
const target = targets[requestedTarget];

if (!target || target !== path.resolve(target)) {
  throw new Error(`unsafe cleanup target: ${target}`);
}

if (!fs.existsSync(target)) {
  process.stdout.write(`Already absent: ${target}`);
  process.exit(0);
}

if (fs.lstatSync(target).isSymbolicLink()) {
  throw new Error(`refusing to remove a symbolic link: ${target}`);
}

fs.rmSync(target, {
  recursive: true,
  force: true,
  maxRetries: 4,
  retryDelay: 150,
});

if (fs.existsSync(target)) {
  throw new Error(`cleanup target still exists: ${target}`);
}

process.stdout.write(`Removed regenerable build: ${target}`);
