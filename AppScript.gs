const DAYS = [
  "Monday",
  "Tuesday",
  "Wednesday",
  "Thursday",
  "Friday",
  "Saturday",
  "Sunday",
];

function doGet(e) {
  // 1) If sheetName is provided, log data instead of returning config
  if (e.parameter && e.parameter.sheetName) {
    return logDataToSheet(e);
  }

  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const cfg = ss.getSheetByName("Config");
  if (!cfg) throw new Error("Sheet “Config” not found");

  // 2) Read Config sheet B2/C2
  const dli = cfg.getRange("B2").getValue();
  const led = cfg.getRange("C2").getValue();

  // 3) Read watering schedule from Config! row 5–7, cols B→H
  const headerRow = cfg.getRange(5, 2, 1, 7).getValues()[0];
  const timeRow = cfg.getRange(6, 2, 1, 7).getValues()[0];
  const durRow = cfg.getRange(7, 2, 1, 7).getValues()[0];

  const watering = {};
  for (let i = 0; i < 7; i++) {
    const dayName = headerRow[i];
    const tcell = timeRow[i];
    const dcell = durRow[i];
    if (dayName && tcell && dcell) {
      // Format the time as "HH:mm"
      let hhmm;
      if (typeof tcell === "string") {
        hhmm = tcell;
      } else {
        hhmm = Utilities.formatDate(
          new Date(tcell),
          ss.getSpreadsheetTimeZone(),
          "HH:mm"
        );
      }
      watering[dayName] = {
        time: hhmm,
        duration: Number(dcell),
      };
    }
  }

  // 4) Return combined JSON
  const out = {
    status: "success",
    dli_target: dli,
    led: led,
    watering: watering,
  };
  return ContentService.createTextOutput(JSON.stringify(out)).setMimeType(
    ContentService.MimeType.JSON
  );
}

function doPost(e) {
  // Update Config!B2/C2 via POST or PUT
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const cfg = ss.getSheetByName("Config");
  const payload = JSON.parse(e.postData.contents || "{}");
  if (payload.dli_target !== undefined)
    cfg.getRange("B2").setValue(payload.dli_target);
  if (payload.led !== undefined) cfg.getRange("C2").setValue(payload.led);
  return ContentService.createTextOutput(
    JSON.stringify({ status: "success" })
  ).setMimeType(ContentService.MimeType.JSON);
}
function doPut(e) {
  return doPost(e);
}

function logDataToSheet(e) {
  // Exactly as before: append a row to the sheet named by ?sheetName=
  const ss = SpreadsheetApp.getActiveSpreadsheet();
  const name = e.parameter.sheetName;
  let sheet = ss.getSheetByName(name);
  if (!sheet) {
    sheet = ss.insertSheet(name);
    sheet.appendRow([
      "Timestamp",
      "PPFD (µmol/m²/s)",
      "DLI (mol/m²/day)",
      "Remaining DLI",
      "Hours Left",
      "LED On Time",
      "Energy (kWh)",
      "Power (W)",
      "Voltage (V)",
      "Current (A)",
      "Date",
    ]);
  }
  const now = new Date();
  sheet.appendRow([
    now,
    e.parameter.ppfd || "",
    e.parameter.dli || "",
    e.parameter.remaining_dli || "",
    e.parameter.hours_left || "",
    e.parameter.ledontime || "",
    e.parameter.energy_kwh || "",
    e.parameter.power || "",
    e.parameter.voltage || "",
    e.parameter.current || "",
    e.parameter.date || "",
  ]);
  return ContentService.createTextOutput("Logged").setMimeType(
    ContentService.MimeType.TEXT
  );
}
