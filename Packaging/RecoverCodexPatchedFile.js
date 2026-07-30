const fs = require("fs");
const path = require("path");

if (process.argv.length !== 5) {
  throw new Error(
    "usage: node RecoverCodexPatchedFile.js <sessions-root> <target-suffix> <output>"
  );
}

const [sessionsRoot, targetSuffixRaw, output] = process.argv.slice(2);
const targetSuffix = targetSuffixRaw.replaceAll("\\", "/").toLowerCase();
let latest;
const matches = [];

function visit(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const fullPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      visit(fullPath);
      continue;
    }
    if (!entry.isFile() || !entry.name.endsWith(".jsonl")) continue;

    const lines = fs.readFileSync(fullPath, "utf8").split(/\r?\n/);
    for (const line of lines) {
      if (!line.includes('"patch_apply_end"') || !line.includes("ChurchDAWServices.h")) {
        continue;
      }
      let record;
      try {
        record = JSON.parse(line);
      } catch {
        continue;
      }
      const payload = record?.payload;
      if (payload?.type !== "patch_apply_end" || payload.success !== true) continue;
      for (const [changedPath, change] of Object.entries(payload.changes ?? {})) {
        const normalized = changedPath.replaceAll("\\", "/").toLowerCase();
        if (!normalized.endsWith(targetSuffix) || typeof change?.content !== "string") {
          if (normalized.endsWith(targetSuffix) && typeof change?.unified_diff === "string") {
            matches.push({
              timestamp: record.timestamp,
              source: fullPath,
              changedPath,
              type: change.type,
              contentBytes: 0,
              diffBytes: Buffer.byteLength(change.unified_diff, "utf8"),
            });
          }
          continue;
        }
        matches.push({
          timestamp: record.timestamp,
          source: fullPath,
          changedPath,
          type: change.type,
          contentBytes: Buffer.byteLength(change.content, "utf8"),
          diffBytes: 0,
        });
        if (!latest || record.timestamp > latest.timestamp) {
          latest = {
            timestamp: record.timestamp,
            source: fullPath,
            changedPath,
            content: change.content,
          };
        }
      }
    }
  }
}

visit(sessionsRoot);
if (output === "--list") {
  process.stdout.write(JSON.stringify(matches.sort((a, b) =>
    a.timestamp.localeCompare(b.timestamp)
  )));
  process.exit(0);
}
if (!latest) throw new Error(`No successful snapshot found for ${targetSuffixRaw}`);
fs.writeFileSync(output, latest.content, "utf8");
process.stdout.write(
  JSON.stringify({
    timestamp: latest.timestamp,
    source: latest.source,
    changedPath: latest.changedPath,
    output,
    bytes: Buffer.byteLength(latest.content, "utf8"),
  })
);
