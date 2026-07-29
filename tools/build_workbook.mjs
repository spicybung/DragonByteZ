import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
const artifactToolPath = path.join(
  process.env.CODEX_PRIMARY_RUNTIME_NODE_MODULES,
  "@oai",
  "artifact-tool",
  "dist",
  "artifact_tool.mjs",
);
const { SpreadsheetFile, Workbook } = await import(artifactToolPath);

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const projectDir = path.resolve(scriptDir, "..");
const analysisDir = path.resolve(
  process.argv[2] ?? path.join(projectDir, "test-output-v0611"),
);
const outputPath = path.resolve(
  process.argv[3] ?? path.join(projectDir, "DragonByteZ_test_asset_index.xlsx"),
);

function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = "";
  let quoted = false;
  for (let index = 0; index < text.length; ++index) {
    const character = text[index];
    if (quoted) {
      if (character === '"' && text[index + 1] === '"') {
        field += '"';
        ++index;
      } else if (character === '"') {
        quoted = false;
      } else {
        field += character;
      }
    } else if (character === '"') {
      quoted = true;
    } else if (character === ",") {
      row.push(field);
      field = "";
    } else if (character === "\n") {
      row.push(field.replace(/\r$/, ""));
      rows.push(row);
      row = [];
      field = "";
    } else {
      field += character;
    }
  }
  if (field || row.length) {
    row.push(field);
    rows.push(row);
  }
  return rows;
}

const levelRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "graphics", "level_tilesets.csv"), "utf8"),
);
const levelRecordRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "graphics", "level_records.csv"), "utf8"),
);
const areaNameRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "graphics", "area_names.csv"), "utf8"),
);
const characterRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "graphics", "character_display_records.csv"), "utf8"),
);
const researchRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "soundtrack", "soundtrack_research.csv"), "utf8"),
);
const rejectedRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "soundtrack", "rejected_false_positive.csv"), "utf8"),
);
const instrumentRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "soundtrack", "instrument_samples.csv"), "utf8"),
);
const sfxRows = parseCsv(
  await fs.readFile(path.join(analysisDir, "soundtrack", "sfx_samples.csv"), "utf8"),
);
const levelMusicRows = parseCsv(
  await fs.readFile(
    path.join(analysisDir, "soundtrack", "level_music", "level_music.csv"),
    "utf8",
  ),
);

const workbook = Workbook.create();
const summary = workbook.worksheets.add("Summary");
const characters = workbook.worksheets.add("Character Display");
const tilesets = workbook.worksheets.add("Level Tilesets");
const levelRecords = workbook.worksheets.add("Level Records");
const areaNames = workbook.worksheets.add("Area Names");
const instruments = workbook.worksheets.add("Instrument Samples");
const effects = workbook.worksheets.add("SFX Samples");
const levelMusic = workbook.worksheets.add("Level Music");
const sounds = workbook.worksheets.add("Sound Research");
const gallery = workbook.worksheets.add("Gallery");

const navy = "#14213D";
const blue = "#2356A8";
const paleBlue = "#EAF1FF";
const paleGreen = "#E8F7ED";
const paleAmber = "#FFF4D6";
const ink = "#172033";
const line = "#C8D3E5";

function setTitle(sheet, range, text) {
  sheet.getRange(range).merge();
  const first = range.split(":")[0];
  sheet.getRange(first).values = [[text]];
  sheet.getRange(range).format = {
    fill: navy,
    font: { bold: true, color: "#FFFFFF", size: 20 },
    verticalAlignment: "center",
  };
}

function typedRows(rows) {
  return rows.map((row, rowIndex) =>
    row.map((value) => {
      if (/^0x[0-9A-F]+$/i.test(value)) return `\u200B${value}`;
      if (rowIndex > 0 && /^-?\d+(?:\.\d+)?$/.test(value)) return Number(value);
      return value;
    }),
  );
}

function addTableSheet(sheet, rows, tableName) {
  const rowCount = rows.length;
  const colCount = rows[0].length;
  sheet.getRangeByIndexes(0, 0, rowCount, colCount).values = typedRows(rows);
  const lastColumn = String.fromCharCode(64 + colCount);
  sheet.getRange(`A1:${lastColumn}1`).format = {
    fill: navy,
    font: { bold: true, color: "#FFFFFF" },
    wrapText: true,
  };
  sheet.tables.add(`A1:${lastColumn}${rowCount}`, true, tableName);
  sheet.getRange(`A1:${lastColumn}${rowCount}`).format.verticalAlignment = "center";
  sheet.getRange(`A1:${lastColumn}${rowCount}`).format.autofitColumns();
  sheet.freezePanes.freezeRows(1);
}

summary.showGridLines = false;
setTitle(summary, "A1:F2", "DragonByteZ — verified LOG2 asset index");
summary.getRange("A4:B4").values = [["ROM", "Value"]];
summary.getRange("A5:B9").values = [
  ["Internal title", "DRAGONBALL Z"],
  ["Game code", "ALFP"],
  ["Region/revision", "Europe Rev 0 test run; USA Rev 0 also supported"],
  ["ROM size", 8388608],
  ["Analyzer", "DragonByteZ 0.6.13 (C++17 GUI/CLI)"],
];
summary.getRange("D4:E4").values = [["Verified extraction", "Count"]];
summary.getRange("D5:D13").values = [
  ["8bpp level tile atlases"],
  ["Decoded tutorial layers"],
  ["Validated level records"],
  ["Localized area names"],
  ["Character-display records"],
  ["Music instrument WAVs"],
  ["Sound-effect WAVs"],
  ["Level-music miniGSF tracks"],
  ["Rejected graphics-as-PCM rows"],
];
summary.getRange("E5").formulas = [["=COUNTA('Level Tilesets'!A2:A169)"]];
summary.getRange("E6").values = [[4]];
summary.getRange("E7").formulas = [["=COUNTA('Level Records'!A2:A310)"]];
summary.getRange("E8").formulas = [["=COUNTA('Area Names'!A2:A71)"]];
summary.getRange("E9").formulas = [["=COUNTA('Character Display'!A2:A23)"]];
summary.getRange("E10").formulas = [["=COUNTA('Instrument Samples'!A2:A102)"]];
summary.getRange("E11").formulas = [["=COUNTA('SFX Samples'!A2:A65)"]];
summary.getRange("E12").formulas = [["=COUNTA('Level Music'!A2:A45)"]];
summary.getRange("E13").values = [[107]];
summary.getRange("A4:B4").format = summary.getRange("D4:E4").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
};
summary.getRange("A5:B9").format = summary.getRange("D5:E13").format = {
  fill: "#F7F9FC",
  font: { color: ink },
  borders: { color: line },
};
summary.getRange("A15:F15").merge();
summary.getRange("A15").values = [["Correction and confidence"]];
summary.getRange("A15:F15").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
};
summary.getRange("A16:F20").merge();
summary.getRange("A16").values = [[
  "The 168 level-table entries are individual compressed 16 KiB 8bpp atlases: one byte per pixel, 256 8×8 tiles, and a 128×128 image using the region-correct 256-colour BG palette. The incorrect 0.6.7 palette-neutral 4bpp grouping has been removed. " +
  "The Gallery shows the corrected atlas contact sheet, the reconstructed local tutorial tileset, and four chunk-decoded tutorial layers. " +
  "DragonByteZ validates 309 map records and recovers 70 area-name records. " +
  "The 107 blocks at 0x006A981C remain rejected indexed graphics. Actual audio now comes from the verified European tables: 101 instrument samples at 0x0037A524 and 64 sound effects at 0x004B8658, each exported at its recorded rate. " +
  "The 44-record BGM table at 0x004047AC is exported as native Webfoot sequences and playable miniGSF selectors. Individual character-frame assembly and sequence opcode decoding remain open research.",
]];
summary.getRange("A16:F20").format = {
  fill: paleAmber,
  font: { color: ink },
  wrapText: true,
  verticalAlignment: "top",
  borders: { color: "#D9B85E" },
};
summary.getRange("A22:C22").values = [["Finding", "Offset", "Confidence"]];
summary.getRange("A23:C34").values = [
  ["168-entry level-atlas pointer table", "\u200B0x00124308", "High"],
  ["First validated map record", "\u200B0x00127C08", "High"],
  ["Default BG RGB555 palette", "\u200B0x006A83F8", "High"],
  ["Default OBJ RGB555 palette", "\u200B0x006A87F8", "High"],
  ["Title bitmap container", "\u200B0x0069C570", "High"],
  ["Title RGB555 palette", "\u200B0x006A85F8", "High"],
  ["Character-display records", "\u200B0x0002A4E8", "High"],
  ["Music instrument sample table", "\u200B0x0037A524", "High"],
  ["Sound-effect sample table", "\u200B0x004B8658", "High"],
  ["Level-music BGM table", "\u200B0x004047AC", "High"],
  ["USA level tile table", "\u200B0x004DF574", "High"],
  ["Rejected indexed-graphic table", "\u200B0x006A981C", "High"],
];
summary.getRange("A22:C22").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
};
summary.getRange("A23:C34").format = { borders: { color: line } };
summary.getRange("A:A").format.columnWidth = 29;
summary.getRange("B:B").format.columnWidth = 42;
summary.getRange("C:C").format.columnWidth = 14;
summary.getRange("D:D").format.columnWidth = 31;
summary.getRange("E:E").format.columnWidth = 16;
summary.getRange("F:F").format.columnWidth = 3;
summary.getRange("16:20").format.rowHeight = 27;
summary.freezePanes.freezeRows(4);

addTableSheet(characters, characterRows, "CharacterDisplayTable");
characters.getRange("B:B").format.columnWidth = 18;
characters.getRange("C:M").format.columnWidth = 14;

addTableSheet(tilesets, levelRows, "LevelTilesetTable");
tilesets.getRange("B:F").format.columnWidth = 18;
tilesets.getRange("G:H").format.columnWidth = 25;
tilesets.getRange("I:I").format.columnWidth = 48;
tilesets.getRange("I2:I169").conditionalFormats.add(
  "containsText",
  { text: "complete", format: { fill: paleGreen, font: { color: "#176B34" } } },
);

addTableSheet(levelRecords, levelRecordRows, "LevelRecordTable");
levelRecords.getRange("B:B").format.columnWidth = 34;
levelRecords.getRange("C:C").format.columnWidth = 18;
levelRecords.getRange("D:F").format.columnWidth = 14;
levelRecords.getRange("G:N").format.columnWidth = 18;

addTableSheet(areaNames, areaNameRows, "AreaNameTable");
areaNames.getRange("A:A").format.columnWidth = 12;
areaNames.getRange("B:K").format.columnWidth = 28;

addTableSheet(instruments, instrumentRows, "InstrumentSampleTable");
instruments.getRange("B:C").format.columnWidth = 18;
instruments.getRange("D:H").format.columnWidth = 18;

addTableSheet(effects, sfxRows, "SfxSampleTable");
effects.getRange("B:C").format.columnWidth = 18;
effects.getRange("D:G").format.columnWidth = 18;

addTableSheet(levelMusic, levelMusicRows, "LevelMusicTable");
levelMusic.getRange("B:D").format.columnWidth = 20;
levelMusic.getRange("E:F").format.columnWidth = 25;
levelMusic.getRange("G:G").format.columnWidth = 18;
levelMusic.getRange("H:H").format.columnWidth = 18;
levelMusic.getRange("I:J").format.columnWidth = 30;

sounds.showGridLines = false;
setTitle(sounds, "A1:I2", "Soundtrack research — verified PCM and BGM tables");
const typedResearchRows = typedRows(researchRows);
const typedRejectedRows = typedRows(rejectedRows);
sounds.getRange("A4:F4").values = [typedResearchRows[0]];
sounds.getRangeByIndexes(4, 0, researchRows.length - 1, researchRows[0].length).values =
  typedResearchRows.slice(1);
sounds.getRange("A4:F4").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
};
sounds.tables.add(`A4:F${researchRows.length + 3}`, true, "SoundResearchTable");
sounds.getRange(`A4:F${researchRows.length + 3}`).format.wrapText = true;
sounds.getRange("A:A").format.columnWidth = 29;
sounds.getRange("B:D").format.columnWidth = 24;
sounds.getRange("E:E").format.columnWidth = 28;
sounds.getRange("F:F").format.columnWidth = 70;
const rejectedTitleRow = researchRows.length + 6;
const rejectedHeaderRow = rejectedTitleRow + 1;
const rejectedLastRow = rejectedHeaderRow + rejectedRows.length - 1;
sounds.getRange(`A${rejectedTitleRow}:I${rejectedTitleRow}`).merge();
sounds.getRange(`A${rejectedTitleRow}`).values = [["Rejected 0x006A981C false-positive rows"]];
sounds.getRange(`A${rejectedTitleRow}:I${rejectedTitleRow}`).format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
};
sounds.getRangeByIndexes(
  rejectedHeaderRow - 1, 0, rejectedRows.length, rejectedRows[0].length,
).values =
  typedRejectedRows;
sounds.getRange(`A${rejectedHeaderRow}:I${rejectedHeaderRow}`).format = {
  fill: navy,
  font: { bold: true, color: "#FFFFFF" },
  wrapText: true,
};
sounds.tables.add(
  `A${rejectedHeaderRow}:I${rejectedLastRow}`, true, "RejectedPcmTable",
);
sounds.getRange("B:C").format.columnWidth = 18;
sounds.getRange("H:H").format.columnWidth = 24;
sounds.getRange("I:I").format.columnWidth = 62;
sounds.freezePanes.freezeRows(4);

gallery.showGridLines = false;
setTitle(gallery, "A1:L2", "Verified Dragon Ball Z art and decoded map layers");
gallery.getRange("A4:L5").merge();
gallery.getRange("A4").values = [[
  "ROM title art uses its recovered RGB555 palette. The tutorial's local tileset is rebuilt from the map's compressed delta-coded global-atlas references. Four layers are then reconstructed from 32×32 chunks using 10-bit local tile IDs and flip bits. Each global atlas is 8bpp and uses the region-correct 256-colour BG palette.",
]];
gallery.getRange("A4:L5").format = {
  fill: paleBlue,
  font: { color: ink },
  wrapText: true,
  verticalAlignment: "center",
};
gallery.getRange("A7:L7").merge();
gallery.getRange("A7").values = [["Verified character/title art"]];
gallery.getRange("A7:L7").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
  horizontalAlignment: "center",
};
gallery.getRange("A31:L31").merge();
gallery.getRange("A31").values = [["Z1A1 opening layers (authoritatively linked through MapEntry variation data)"]];
gallery.getRange("A31:L31").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
  horizontalAlignment: "center",
};

async function imageDataUrl(filename) {
  const bytes = await fs.readFile(filename);
  return `data:image/png;base64,${bytes.toString("base64")}`;
}
gallery.images.add({
  dataUrl: await imageDataUrl(
    path.join(analysisDir, "graphics", "character_art", "title_character_art.png"),
  ),
  anchor: { from: { row: 7, col: 3 }, extent: { widthPx: 720, heightPx: 480 } },
});
gallery.images.add({
  dataUrl: await imageDataUrl(
    path.join(
      analysisDir,
      "graphics",
      "level_previews",
      "opening",
      "Z1A1_layers_contact_sheet.png",
    ),
  ),
  anchor: { from: { row: 31, col: 1 }, extent: { widthPx: 900, heightPx: 900 } },
});
gallery.getRange("A74:L74").merge();
gallery.getRange("A74").values = [["8bpp tile-atlas checks — 256 tiles per 128×128 atlas"]];
gallery.getRange("A74:L74").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
  horizontalAlignment: "center",
};
gallery.images.add({
  dataUrl: await imageDataUrl(
    path.join(
      analysisDir,
      "graphics",
      "level_tilesets",
      "tileset_03.png",
    ),
  ),
  anchor: { from: { row: 75, col: 2 }, extent: { widthPx: 256, heightPx: 256 } },
});
gallery.images.add({
  dataUrl: await imageDataUrl(
    path.join(
      analysisDir,
      "graphics",
      "level_tilesets",
      "tileset_27.png",
    ),
  ),
  anchor: { from: { row: 75, col: 7 }, extent: { widthPx: 256, heightPx: 256 } },
});
gallery.getRange("A88:L88").merge();
gallery.getRange("A88").values = [["All 168 corrected 8bpp tile atlases"]];
gallery.getRange("A88:L88").format = {
  fill: blue,
  font: { bold: true, color: "#FFFFFF" },
  horizontalAlignment: "center",
};
gallery.images.add({
  dataUrl: await imageDataUrl(
    path.join(analysisDir, "graphics", "level_tileset_contact_sheet.png"),
  ),
  anchor: { from: { row: 89, col: 1 }, extent: { widthPx: 900, heightPx: 392 } },
});
gallery.getRange("A:L").format.columnWidth = 15;
gallery.getRange("8:110").format.rowHeight = 22;
gallery.freezePanes.freezeRows(7);

const inspect = await workbook.inspect({
  kind: "workbook,sheet,table,formula",
  maxChars: 14000,
  tableMaxRows: 7,
  tableMaxCols: 20,
});
await fs.writeFile(
  path.join(analysisDir, "workbook_inspect.ndjson"),
  inspect.ndjson ?? JSON.stringify(inspect, null, 2),
);

for (const sheetName of [
  "Summary",
  "Character Display",
  "Level Tilesets",
  "Level Records",
  "Area Names",
  "Instrument Samples",
  "SFX Samples",
  "Level Music",
  "Sound Research",
  "Gallery",
]) {
  const preview = await workbook.render({
    sheetName,
    autoCrop: "all",
    scale: sheetName === "Gallery" ? 0.45 : 0.8,
    format: "png",
  });
  await fs.writeFile(
    path.join(
      analysisDir,
      `workbook_${sheetName.toLowerCase().replaceAll(" ", "_")}.png`,
    ),
    new Uint8Array(await preview.arrayBuffer()),
  );
}

const formulaErrors = await workbook.inspect({
  kind: "match",
  searchTerm: "#REF!|#DIV/0!|#VALUE!|#NAME\\?|#N/A",
  options: { useRegex: true, maxResults: 300 },
  summary: "final formula error scan",
});
await fs.writeFile(
  path.join(analysisDir, "workbook_formula_errors.ndjson"),
  formulaErrors.ndjson ?? JSON.stringify(formulaErrors, null, 2),
);

const xlsx = await SpreadsheetFile.exportXlsx(workbook);
await xlsx.save(outputPath);
console.log(outputPath);
