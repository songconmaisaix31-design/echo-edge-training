import fs from "node:fs/promises";
import { SpreadsheetFile, Workbook } from "@oai/artifact-tool";

const [inputPath, outputPath] = process.argv.slice(2);
if (!inputPath || !outputPath) {
  throw new Error("用法：node build_review_workbook.mjs <review-input.json> <医生复核.xlsx>");
}
const { batch_id: batchId, rows } = JSON.parse(await fs.readFile(inputPath, "utf8"));
if (typeof batchId !== "string" || !batchId.trim() || !Array.isArray(rows) || rows.length !== 20) {
  throw new Error("批次需要包含名称和 20 条复核记录");
}
const ids = rows.map(row => row.review_id);
if (new Set(ids).size !== 20 || ids.some(id => typeof id !== "string" || !/^REV-\d{2}$/.test(id))) {
  throw new Error("复核编号必须唯一，格式为 REV- 加两位数字");
}

const book = Workbook.create();
const sheet = book.worksheets.add("医生复核");
sheet.showGridLines = false;
const font = "Microsoft YaHei";
const text = "#193245";
const navy = "#244760";
const columns = [
  ["复核编号", "review_id", 15], ["开始（毫秒）", "start_ms", 17], ["结束（毫秒）", "end_ms", 17],
  ["医生判断切面", "view", 17], ["PSAX 层面", "psax_level", 22], ["采集质量", "quality", 16],
  ["可见或缺失的目标结构", "structure_notes", 32], ["质量或不确定原因", "reason", 29],
  ["复核状态", "review_status", 16],
];
const lastRow = 4 + rows.length;

function style(range, size, color, bold = false) {
  range.format.font.name = font;
  range.format.font.size = size;
  range.format.font.color = color;
  range.format.font.bold = bold;
  range.format.verticalAlignment = "center";
}

sheet.getRange("A1").values = [["儿童心超切面与采集质量复核"]];
sheet.getRange("A2").values = [[`批次：${batchId}`]];
sheet.getRange("A1:I1").format.rowHeight = 28;
sheet.getRange("A2:I2").format.rowHeight = 22;
style(sheet.getRange("A1"), 16, text, true);
style(sheet.getRange("A2"), 10, "#536B7B");

const headings = sheet.getRange("A3:I3");
headings.values = [columns.map(column => column[0])];
headings.format.fill = navy;
headings.format.rowHeight = 25;
style(headings, 10, "#FFFFFF", true);
const keys = sheet.getRange("A4:I4");
keys.values = [columns.map(column => column[1])];
keys.format.fill = "#EAF2F7";
style(keys, 9, "#536B7B");

for (const [index, column] of columns.entries()) {
  sheet.getCell(0, index).format.columnWidth = column[2];
}
sheet.getRange(`A5:A${lastRow}`).values = ids.map(id => [id]);
sheet.getRange(`I5:I${lastRow}`).values = ids.map(() => ["待复核"]);
sheet.getRange(`A5:I${lastRow}`).format.rowHeight = 26;
style(sheet.getRange(`A5:I${lastRow}`), 10, text);
sheet.getRange(`A5:A${lastRow}`).format.fill = "#EAF2F7";
sheet.getRange(`B5:I${lastRow}`).format.fill = "#FFF9E9";
sheet.getRange(`B5:C${lastRow}`).setNumberFormat("0");
sheet.getRange(`G5:H${lastRow}`).format.wrapText = true;
sheet.getRange(`D5:D${lastRow}`).dataValidation = { rule: { type: "list", values: ["A4C", "PSAX", "其他", "过渡", "无法判断"] } };
sheet.getRange(`F5:F${lastRow}`).dataValidation = { rule: { type: "list", values: ["可评估", "需补扫", "无法评估"] } };
sheet.getRange(`I5:I${lastRow}`).dataValidation = { rule: { type: "list", values: ["待复核", "已确认"] } };
sheet.freezePanes.freezeRows(4);

const guide = book.worksheets.add("填写说明");
guide.showGridLines = false;
guide.getRange("A1").values = [["20 段试标填写说明"]];
style(guide.getRange("A1"), 16, text, true);
guide.getRange("A1:B1").format.rowHeight = 28;
guide.getRange("A1").format.columnWidth = 22;
guide.getRange("B1").format.columnWidth = 91;
guide.getRange("A3:B3").values = [["事项", "如何填写"]];
guide.getRange("A3:B3").format.fill = navy;
style(guide.getRange("A3:B3"), 10, "#FFFFFF", true);
const notes = [
  ["找到影像", "按 REV 编号向项目联系人取得已授权的对应影像。编号用于关联本批复核，请保留编号、批次和表头。"],
  ["独立复核", "两位医生可各自填写一份。先记录各自判断，再回看分歧片段并保留最终复核意见。"],
  ["时间区间", "整段适用同一判定时，开始和结束均留空。只评其中一段时，以首帧为 0，填写整数毫秒。开始包含，结束排除。每个编号填一行。"],
  ["切面", "按图像独立判断。选 PSAX 时，若能识别具体短轴层面请填备注。过渡或无法判断时请说明原因。"],
  ["采集质量", "按负责人确认的切面、成像模式和目标结构标准填写。结构显示充分时，异常解剖也可评估。尚无标准或不能判断时，留空质量并说明原因。"],
  ["复核状态", "看完并确认判断后改为已确认。仍需回看或讨论的行保留待复核。完成后将 Excel 通过项目约定渠道交回。"],
];
const body = guide.getRange(`A4:B${3 + notes.length}`);
body.values = notes;
body.format.wrapText = true;
body.format.rowHeight = 40;
style(body, 10, text);
guide.getRange(`A4:A${3 + notes.length}`).format.fill = "#EAF2F7";

book.recalculate();
const check = await book.inspect({ kind: "table", range: "医生复核!A2:I6", include: "values", tableMaxRows: 5, tableMaxCols: 9 });
console.log(check.ndjson);
const workbook = await SpreadsheetFile.exportXlsx(book);
await workbook.save(outputPath);
