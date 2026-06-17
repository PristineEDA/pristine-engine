#!/usr/bin/env node

import { mkdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { brotliDecompressSync, inflateRawSync } from "node:zlib";

const sourceUrl =
  "https://github.com/TinyTapeout/tinytapeout-sky-25a/archive/refs/heads/main.zip";
const gdsBrEntrySuffix = "/projects/tt_um_tt_tinyQV/tt_um_tt_tinyQV.gds.br";
const zipPath = resolve(
  ".deps",
  "src",
  "tinytapeout-sky-25a",
  "tinytapeout-sky-25a-main.zip",
);
const zipTempPath = `${zipPath}.tmp`;
const outputPath = resolve(
  ".deps",
  "src",
  "tinytapeout-sky-25a",
  "projects",
  "tt_um_tt_tinyQV",
  "tt_um_tt_tinyQV.gds",
);

async function existsNonEmpty(path) {
  try {
    const info = await stat(path);
    return info.isFile() && info.size > 0;
  } catch {
    return false;
  }
}

function systemDownloadCommands(url, outputPath) {
  const powershellScript =
    "$ProgressPreference = 'SilentlyContinue'; " +
    "Invoke-WebRequest -Uri $args[0] -OutFile $args[1] -MaximumRedirection 10";
  return [
    {
      command: "curl",
      args: [
        "--location",
        "--fail",
        "--silent",
        "--show-error",
        "--retry",
        "5",
        "--retry-delay",
        "2",
        "--connect-timeout",
        "30",
        "--output",
        outputPath,
        url,
      ],
    },
    {
      command: "pwsh",
      args: ["-NoProfile", "-Command", powershellScript, url, outputPath],
    },
    {
      command: "powershell.exe",
      args: [
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-Command",
        powershellScript,
        url,
        outputPath,
      ],
    },
    {
      command: "wget",
      args: ["--tries=5", "--timeout=30", "--output-document", outputPath, url],
    },
  ];
}

function runSystemCommand(command, args) {
  return new Promise((resolveRun) => {
    const child = spawn(command, args, {
      stdio: "inherit",
      windowsHide: true,
    });
    child.on("error", (error) => {
      resolveRun({ ok: false, message: `${command}: ${error.message}` });
    });
    child.on("close", (code, signal) => {
      if (code === 0) {
        resolveRun({ ok: true, message: `${command}: ok` });
        return;
      }
      const suffix = signal ? `signal ${signal}` : `exit code ${code}`;
      resolveRun({ ok: false, message: `${command}: ${suffix}` });
    });
  });
}

async function downloadWithSystemTool(url, outputPath) {
  let lastMessage = "no downloader was attempted";
  const commands = systemDownloadCommands(url, outputPath);
  for (const { command, args } of commands) {
    await rm(outputPath, { force: true });
    console.log(`Downloading with ${command}: ${url}`);
    const result = await runSystemCommand(command, args);
    if (result.ok && (await existsNonEmpty(outputPath))) {
      return;
    }
    lastMessage = result.message;
    console.warn(`Download with ${command} failed: ${result.message}`);
  }
  throw new Error(`Could not download ${url}; last failure: ${lastMessage}`);
}

async function loadRepositoryZip() {
  if (await existsNonEmpty(zipPath)) {
    console.log(`Reusing TinyTapeout repo zip: ${zipPath}`);
    return readFile(zipPath);
  }

  try {
    await mkdir(dirname(zipPath), { recursive: true });
    await downloadWithSystemTool(sourceUrl, zipTempPath);
    const zip = await readFile(zipTempPath);
    findZipEntry(zip, gdsBrEntrySuffix);
    await rename(zipTempPath, zipPath);
    console.log(`Wrote TinyTapeout repo zip: ${zipPath} (${zip.length} bytes)`);
    return zip;
  } catch (error) {
    await rm(zipTempPath, { force: true });
    throw error;
  }
}

function findEndOfCentralDirectory(zip) {
  const signature = 0x06054b50;
  const minOffset = Math.max(0, zip.length - 0xffff - 22);
  for (let offset = zip.length - 22; offset >= minOffset; offset -= 1) {
    if (zip.readUInt32LE(offset) === signature) {
      return offset;
    }
  }
  throw new Error("Could not find zip end of central directory");
}

function findZipEntry(zip, suffix) {
  const eocd = findEndOfCentralDirectory(zip);
  const centralDirectorySize = zip.readUInt32LE(eocd + 12);
  const centralDirectoryOffset = zip.readUInt32LE(eocd + 16);
  if (centralDirectorySize === 0xffffffff || centralDirectoryOffset === 0xffffffff) {
    throw new Error("Zip64 archives are not supported by this fixture fetcher");
  }
  const end = centralDirectoryOffset + centralDirectorySize;
  for (let offset = centralDirectoryOffset; offset < end; ) {
    if (zip.readUInt32LE(offset) !== 0x02014b50) {
      throw new Error(`Bad central directory entry at offset ${offset}`);
    }
    const compressionMethod = zip.readUInt16LE(offset + 10);
    const compressedSize = zip.readUInt32LE(offset + 20);
    const uncompressedSize = zip.readUInt32LE(offset + 24);
    const nameLength = zip.readUInt16LE(offset + 28);
    const extraLength = zip.readUInt16LE(offset + 30);
    const commentLength = zip.readUInt16LE(offset + 32);
    const localHeaderOffset = zip.readUInt32LE(offset + 42);
    const nameStart = offset + 46;
    const name = zip.subarray(nameStart, nameStart + nameLength).toString("utf8");
    if (name.endsWith(suffix)) {
      return {
        name,
        compressionMethod,
        compressedSize,
        uncompressedSize,
        localHeaderOffset,
      };
    }
    offset = nameStart + nameLength + extraLength + commentLength;
  }
  throw new Error(`Could not find ${suffix} in downloaded repository zip`);
}

function extractZipEntry(zip, entry) {
  const offset = entry.localHeaderOffset;
  if (zip.readUInt32LE(offset) !== 0x04034b50) {
    throw new Error(`Bad local file header for ${entry.name}`);
  }
  const nameLength = zip.readUInt16LE(offset + 26);
  const extraLength = zip.readUInt16LE(offset + 28);
  const dataStart = offset + 30 + nameLength + extraLength;
  const compressed = zip.subarray(dataStart, dataStart + entry.compressedSize);
  if (entry.compressionMethod === 0) {
    return Buffer.from(compressed);
  }
  if (entry.compressionMethod === 8) {
    const inflated = inflateRawSync(compressed);
    if (inflated.length !== entry.uncompressedSize) {
      throw new Error(
        `Unexpected uncompressed size for ${entry.name}: ${inflated.length} != ${entry.uncompressedSize}`,
      );
    }
    return inflated;
  }
  throw new Error(`Unsupported zip compression method ${entry.compressionMethod} for ${entry.name}`);
}

if (await existsNonEmpty(outputPath)) {
  console.log(`TT tinyQV GDS already exists: ${outputPath}`);
  process.exit(0);
}

const zip = await loadRepositoryZip();
const entry = findZipEntry(zip, gdsBrEntrySuffix);
const compressed = extractZipEntry(zip, entry);
const decompressed = brotliDecompressSync(compressed);
await mkdir(dirname(outputPath), { recursive: true });
await writeFile(outputPath, decompressed);
console.log(`Extracted ${entry.name}`);
console.log(`Wrote ${outputPath} (${decompressed.length} bytes)`);
