const fs = require("fs");
const { spawnSync } = require("child_process");

if (process.argv.length < 7) {
  throw new Error(
    "usage: node ReplayCodexUnifiedDiffs.js <base> <output> <target-suffix> <session>::<after-iso> [<session>::<after-iso> ...]"
  );
}

const [basePath, outputPath, targetSuffixRaw, ...sessionSpecs] =
  process.argv.slice(2);
const targetSuffix = targetSuffixRaw.replaceAll("\\", "/").toLowerCase();

function matchingPatchEvents(sessionPath, afterIso) {
  const result = spawnSync(
    "rg",
    [
      "--no-line-number",
      '"patch_apply_end".*ChurchDAWServices\\.h',
      sessionPath,
    ],
    {
      encoding: "utf8",
      maxBuffer: 64 * 1024 * 1024,
      windowsHide: true,
    }
  );

  if (result.status !== 0 && result.status !== 1) {
    throw new Error(
      `rg failed for ${sessionPath}: ${result.stderr || result.status}`
    );
  }

  const events = [];
  for (const line of result.stdout.split(/\r?\n/)) {
    if (!line) continue;

    let record;
    try {
      record = JSON.parse(line);
    } catch {
      continue;
    }

    const payload = record?.payload;
    if (
      payload?.type !== "patch_apply_end" ||
      payload.success !== true ||
      record.timestamp <= afterIso
    ) {
      continue;
    }

    for (const [changedPath, change] of Object.entries(
      payload.changes ?? {}
    )) {
      const normalized = changedPath.replaceAll("\\", "/").toLowerCase();
      if (
        normalized.endsWith(targetSuffix) &&
        typeof change?.unified_diff === "string"
      ) {
        events.push({
          timestamp: record.timestamp,
          callId: payload.call_id,
          sessionPath,
          diff: change.unified_diff,
        });
      }
    }
  }

  return events;
}

function parseHunks(diff) {
  const lines = diff.replaceAll("\r\n", "\n").split("\n");
  const hunks = [];
  let current;

  for (const line of lines) {
    const header = line.match(
      /^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/
    );
    if (header) {
      current = {
        oldStart: Number(header[1]),
        newStart: Number(header[3]),
        oldLines: [],
        newLines: [],
      };
      hunks.push(current);
      continue;
    }

    if (!current || line.startsWith("\\ No newline at end of file")) {
      continue;
    }

    const prefix = line[0];
    const body = line.slice(1);
    if (prefix === " " || prefix === "-") current.oldLines.push(body);
    if (prefix === " " || prefix === "+") current.newLines.push(body);
  }

  return hunks;
}

function findSequence(lines, sequence, preferredIndex) {
  if (sequence.length === 0) {
    return Math.max(0, Math.min(preferredIndex, lines.length));
  }

  const candidates = [];
  outer: for (let i = 0; i + sequence.length <= lines.length; ++i) {
    for (let j = 0; j < sequence.length; ++j) {
      if (lines[i + j] !== sequence[j]) continue outer;
    }
    candidates.push(i);
  }

  if (candidates.length === 0) return -1;
  candidates.sort(
    (left, right) =>
      Math.abs(left - preferredIndex) - Math.abs(right - preferredIndex)
  );
  return candidates[0];
}

function applyDiff(lines, diff, label) {
  let lineOffset = 0;
  for (const hunk of parseHunks(diff)) {
    const preferred = Math.max(0, hunk.oldStart - 1 + lineOffset);
    let index = findSequence(lines, hunk.oldLines, preferred);

    if (index < 0) {
      const alreadyApplied = findSequence(
        lines,
        hunk.newLines,
        Math.max(0, hunk.newStart - 1 + lineOffset)
      );
      if (alreadyApplied >= 0) continue;
      throw new Error(
        `Cannot apply ${label} hunk at old line ${hunk.oldStart}`
      );
    }

    lines.splice(index, hunk.oldLines.length, ...hunk.newLines);
    lineOffset += hunk.newLines.length - hunk.oldLines.length;
  }
}

const allEvents = [];
for (const spec of sessionSpecs) {
  const separator = spec.lastIndexOf("::");
  if (separator < 1) throw new Error(`invalid session specification: ${spec}`);
  const sessionPath = spec.slice(0, separator);
  const afterIso = spec.slice(separator + 2);
  allEvents.push(...matchingPatchEvents(sessionPath, afterIso));
}

allEvents.sort((left, right) => left.timestamp.localeCompare(right.timestamp));

const base = fs.readFileSync(basePath, "utf8");
const eol = base.includes("\r\n") ? "\r\n" : "\n";
const hadFinalEol = /\r?\n$/.test(base);
const lines = base.replaceAll("\r\n", "\n").replace(/\n$/, "").split("\n");

for (const event of allEvents) {
  applyDiff(lines, event.diff, `${event.timestamp} ${event.callId}`);
}

fs.writeFileSync(
  outputPath,
  lines.join(eol) + (hadFinalEol ? eol : ""),
  "utf8"
);

process.stdout.write(
  JSON.stringify(
    {
      basePath,
      outputPath,
      eventCount: allEvents.length,
      events: allEvents.map(({ timestamp, callId, sessionPath, diff }) => ({
        timestamp,
        callId,
        sessionPath,
        diffBytes: Buffer.byteLength(diff, "utf8"),
      })),
      outputBytes: fs.statSync(outputPath).size,
    },
    null,
    2
  )
);
