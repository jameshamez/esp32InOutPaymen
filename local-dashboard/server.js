const http = require("http");
const fs = require("fs");
const path = require("path");
const { URL, URLSearchParams } = require("url");
const QRCode = require("qrcode");
const { OmiseClient } = require("./omise");

function loadEnvFile() {
  const envFile = path.join(__dirname, "..", ".env");
  if (!fs.existsSync(envFile)) return;
  for (const line of fs.readFileSync(envFile, "utf8").split(/\r?\n/)) {
    const match = line.match(/^\s*([A-Z][A-Z0-9_]*)\s*=\s*(.*)\s*$/);
    if (!match || process.env[match[1]] !== undefined) continue;
    process.env[match[1]] = match[2].replace(/^(['"])(.*)\1$/, "$2");
  }
}

loadEnvFile();

const PORT = Number(process.env.DASHBOARD_PORT || 3001);
const DEFAULT_DEVICE_URL = process.env.ESP32_BASE_URL || "http://paymentesp.local";
const DEFAULT_PROMPTPAY_ID = process.env.PROMPTPAY_ID || "0812345678";
const DEFAULT_WEBHOOK_URL = process.env.WEBHOOK_URL || "";
const DEFAULT_PPOINTS_STATION_ID = process.env.PPOINTS_STATION_ID || "P-24001";
const DEFAULT_PPOINTS_BANK_ID = process.env.PPOINTS_BANK_ID || "X-9786";
const PPOINTS_BASE_URL = process.env.PPOINTS_BASE_URL || "https://p-points.com/sms_payin_rd.php";
const CONFIG_FILE = process.env.DASHBOARD_CONFIG_FILE || path.join(__dirname, "data", "config.json");
const PAYMENTS_FILE = process.env.PAYMENTS_FILE || path.join(__dirname, "data", "payments.json");
const EVIDENCE_FILE = path.join(__dirname, "..", "docs", "evidence", "test-evidence.html");
const PUBLIC_BASE_URL = String(process.env.PUBLIC_BASE_URL || "").replace(/\/$/, "");
const omise = new OmiseClient({ secretKey: process.env.OMISE_SECRET_KEY || "" });
const OMISE_MIN_AMOUNT_MINOR = 2000;
const OMISE_MAX_AMOUNT_MINOR = 15000000;

function normalizeWebhookUrl(value) {
  const text = String(value || "").trim();
  if (!text) return "";
  const parsed = new URL(text);
  if (!['http:', 'https:'].includes(parsed.protocol) || text.length > 240) {
    throw new Error("Webhook URL must start with http:// or https://");
  }
  return text;
}

function loadDashboardConfig() {
  try {
    const saved = JSON.parse(fs.readFileSync(CONFIG_FILE, "utf8"));
    normalizePromptPayTarget(saved.promptPayId);
    return {
      promptPayId: digitsOnly(saved.promptPayId),
      webhookUrl: normalizeWebhookUrl(saved.webhookUrl || DEFAULT_WEBHOOK_URL),
      ppointsStationId: String(saved.ppointsStationId || DEFAULT_PPOINTS_STATION_ID).trim(),
      ppointsBankId: String(saved.ppointsBankId || DEFAULT_PPOINTS_BANK_ID).trim(),
    };
  } catch {
    return {
      promptPayId: DEFAULT_PROMPTPAY_ID,
      webhookUrl: DEFAULT_WEBHOOK_URL,
      ppointsStationId: DEFAULT_PPOINTS_STATION_ID,
      ppointsBankId: DEFAULT_PPOINTS_BANK_ID,
    };
  }
}

function validatePpointsId(value, name) {
  const text = String(value || "").trim();
  if (!/^[A-Za-z0-9_-]{1,40}$/.test(text)) {
    throw new Error(`${name} must contain only letters, numbers, dash, or underscore`);
  }
  return text;
}

function saveDashboardConfig({ promptPayId, webhookUrl, ppointsStationId, ppointsBankId }) {
  normalizePromptPayTarget(promptPayId);
  const config = {
    promptPayId: digitsOnly(promptPayId),
    webhookUrl: normalizeWebhookUrl(webhookUrl),
    ppointsStationId: validatePpointsId(ppointsStationId || dashboardConfig.ppointsStationId, "P-Points stn_id"),
    ppointsBankId: validatePpointsId(ppointsBankId || dashboardConfig.ppointsBankId, "P-Points bank_id"),
  };
  fs.mkdirSync(path.dirname(CONFIG_FILE), { recursive: true });
  const temporaryFile = CONFIG_FILE + ".tmp";
  fs.writeFileSync(temporaryFile, JSON.stringify(config, null, 2) + "\n", "utf8");
  fs.renameSync(temporaryFile, CONFIG_FILE);
  return config;
}

let dashboardConfig = loadDashboardConfig();
const webhookEvents = [];

function loadPayments() {
  try {
    const data = JSON.parse(fs.readFileSync(PAYMENTS_FILE, "utf8"));
    return data && typeof data === "object" ? data : {};
  } catch {
    return {};
  }
}

function savePayments() {
  fs.mkdirSync(path.dirname(PAYMENTS_FILE), { recursive: true });
  const temporaryFile = PAYMENTS_FILE + ".tmp";
  fs.writeFileSync(temporaryFile, JSON.stringify(payments, null, 2) + "\n", "utf8");
  fs.renameSync(temporaryFile, PAYMENTS_FILE);
}

const payments = loadPayments();

function isAllowedTarget(targetUrl) {
  if (!["http:", "https:"].includes(targetUrl.protocol)) {
    return false;
  }

  const host = targetUrl.hostname.toLowerCase();
  if (host === "paymentesp.local") {
    return true;
  }
  if (host === "localhost" || host === "::1" || host === "127.0.0.1") {
    return true;
  }

  const parts = host.split(".").map((part) => Number(part));
  if (parts.length !== 4 || parts.some((part) => !Number.isInteger(part) || part < 0 || part > 255)) {
    return false;
  }

  return (
    parts[0] === 10 ||
    (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) ||
    (parts[0] === 192 && parts[1] === 168)
  );
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => resolve(Buffer.concat(chunks)));
    req.on("error", reject);
  });
}

function send(res, status, body, headers = {}) {
  res.writeHead(status, {
    "Cache-Control": "no-store",
    ...headers,
  });
  res.end(body);
}

function sendJson(res, status, data) {
  send(res, status, JSON.stringify(data), {
    "Content-Type": "application/json; charset=utf-8",
  });
}

function digitsOnly(value) {
  return String(value || "").replace(/\D/g, "");
}

function htmlEscape(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[char]);
}

function tlv(id, value) {
  const text = String(value);
  return id + String(text.length).padStart(2, "0") + text;
}

function normalizePromptPayTarget(input) {
  const digits = digitsOnly(input);

  if (digits.length === 10 && digits.startsWith("0")) {
    return { value: "0066" + digits.slice(1), type: "phone", tag: "01" };
  }

  if (digits.length === 11 && digits.startsWith("66")) {
    return { value: "00" + digits, type: "phone", tag: "01" };
  }

  if (digits.length === 13) {
    return { value: digits, type: "national_id", tag: "02" };
  }

  if (digits.length === 15) {
    return { value: digits, type: "e_wallet", tag: "03" };
  }

  throw new Error("PromptPay ID must be a Thai phone number, 13-digit national/tax ID, or 15-digit e-wallet ID");
}

function crc16CcittFalse(data) {
  let crc = 0xffff;
  for (const char of data) {
    crc ^= char.charCodeAt(0) << 8;
    for (let i = 0; i < 8; i += 1) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) : crc << 1;
      crc &= 0xffff;
    }
  }
  return crc;
}

function formatMoney(amount) {
  const value = Number(amount);
  if (!Number.isFinite(value) || value <= 0) {
    return "";
  }
  return value.toFixed(2);
}

function buildPromptPayPayload({ promptpay, amount = 0, ref = "", dynamic = false }) {
  const target = normalizePromptPayTarget(promptpay);
  const merchant = tlv("00", "A000000677010111") + tlv(target.tag, target.value);

  let payload = "";
  payload += tlv("00", "01");
  payload += tlv("01", dynamic ? "12" : "11");
  payload += tlv("29", merchant);
  payload += tlv("58", "TH");
  payload += tlv("53", "764");

  const amountText = formatMoney(amount);
  if (dynamic && !amountText) {
    throw new Error("Amount must be greater than 0");
  }
  if (amountText) {
    payload += tlv("54", amountText);
  }

  if (ref) {
    payload += tlv("62", tlv("05", String(ref).slice(0, 25)));
  }

  payload += "6304";
  payload += crc16CcittFalse(payload).toString(16).toUpperCase().padStart(4, "0");

  return {
    mode: dynamic ? "dynamic" : "static",
    promptPayId: target.value,
    targetType: target.type,
    amount: amountText,
    reference: ref,
    payload,
    createdAtMs: Date.now(),
    createdAtEpochMs: Date.now(),
    createdAt: new Date().toISOString(),
    webhookStatus: 0,
    source: "local-dashboard",
  };
}

async function receiveWebhook(req, res) {
  try {
    const body = (await readBody(req)).toString("utf8");
    const event = JSON.parse(body);
    if (!event || typeof event !== "object" || !event.mode || !event.payload) {
      throw new Error("QR event JSON must include mode and payload");
    }
    const received = { ...event, receivedAt: new Date().toISOString() };
    webhookEvents.push(received);
    if (webhookEvents.length > 50) webhookEvents.shift();
    sendJson(res, 202, { ok: true, receivedAt: received.receivedAt, eventCount: webhookEvents.length });
  } catch (error) {
    sendJson(res, 400, { error: error.message });
  }
}

async function readRequestData(req) {
  const raw = (await readBody(req)).toString("utf8");
  if ((req.headers["content-type"] || "").includes("application/json")) {
    return raw ? JSON.parse(raw) : {};
  }
  return Object.fromEntries(new URLSearchParams(raw));
}

function parseOmiseAmount(value) {
  const amount = Number(value);
  const amountMinor = Math.round(amount * 100);
  if (!Number.isFinite(amount) || amountMinor < OMISE_MIN_AMOUNT_MINOR || amountMinor > OMISE_MAX_AMOUNT_MINOR) {
    throw new Error("Omise PromptPay amount must be between 20.00 and 150000.00 THB");
  }
  return amountMinor;
}

function paymentPublicView(payment) {
  return {
    chargeId: payment.chargeId,
    status: payment.status,
    paid: payment.paid,
    amountMinor: payment.amountMinor,
    amount: payment.amount,
    currency: payment.currency,
    reference: payment.reference,
    qrImageUrl: payment.qrImageUrl,
    livemode: payment.livemode,
    createdAt: payment.createdAt,
    paidAt: payment.paidAt,
    expiresAt: payment.expiresAt,
    deviceNotified: Boolean(payment.deviceNotified),
    deviceNotification: payment.deviceNotification || null,
  };
}

function updatePayment(charge, previous = {}) {
  if (previous.amountMinor && previous.amountMinor !== charge.amountMinor) {
    throw new Error("Omise charge amount does not match the stored payment");
  }
  if (previous.reference && charge.reference && previous.reference !== charge.reference) {
    throw new Error("Omise charge reference does not match the stored payment");
  }
  const payment = {
    ...previous,
    ...charge,
    deviceUrl: previous.deviceUrl || DEFAULT_DEVICE_URL,
    updatedAt: new Date().toISOString(),
  };
  payments[payment.chargeId] = payment;
  savePayments();
  return payment;
}

async function notifyDeviceOfPayment(payment) {
  if (!payment.paid || payment.deviceNotified) return payment;

  let target;
  try {
    target = new URL(payment.deviceUrl || DEFAULT_DEVICE_URL);
  } catch {
    throw new Error("Invalid ESP32 device URL stored for payment");
  }
  if (!isAllowedTarget(target)) {
    throw new Error("ESP32 payment target must be localhost or a private LAN IP");
  }

  const endpoint = new URL("/api/payment", target);
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 5000);
  try {
    const response = await fetch(endpoint, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({
        paymentId: payment.chargeId,
        status: "paid",
        amount: payment.amount,
        ref: payment.reference,
      }),
      signal: controller.signal,
    });
    const text = await response.text();
    let data;
    try { data = JSON.parse(text); } catch { data = { raw: text }; }
    payment.deviceNotification = { ok: response.ok, status: response.status, data };
    payment.deviceNotified = response.ok;
  } catch (error) {
    payment.deviceNotification = { ok: false, error: error.name === "AbortError" ? "Device request timed out" : error.message };
  } finally {
    clearTimeout(timeout);
  }
  payments[payment.chargeId] = payment;
  savePayments();
  return payment;
}

function parsePpointsResponse(text) {
  const raw = String(text || "").trim();
  let data;
  try {
    data = JSON.parse(raw);
  } catch {
    const res = raw.match(/"?RES"?\s*:?\s*"?([A-Z]+)"?/i);
    const stn = raw.match(/"?stn_id"?\s*:?\s*"?([A-Za-z0-9_-]+)"?/i);
    const bank = raw.match(/"?bank_id"?\s*:?\s*"?([A-Za-z0-9_-]+)"?/i);
    const amt = raw.match(/"?amt"?\s*:?\s*\[\s*"?([0-9]+(?:\.[0-9]+)?)"?\s*,\s*"?([0-9]+)"?/i);
    data = {
      RES: res ? res[1] : "",
      stn_id: stn ? stn[1] : "",
      bank_id: bank ? bank[1] : "",
      amt: amt ? [amt[1], amt[2]] : [],
      raw,
    };
  }

  const status = String(data.RES || data.res || "").toUpperCase();
  const amount = formatMoney(Array.isArray(data.amt) ? data.amt[0] : data.amount);
  const count = String(Array.isArray(data.amt) ? data.amt[1] : data.count || "").trim();
  return {
    ok: status === "OK",
    status,
    stationId: String(data.stn_id || data.stationId || ""),
    bankId: String(data.bank_id || data.bankId || ""),
    amount,
    count,
    raw: data.raw || data,
  };
}

async function checkPpointsPayin({ stationId, bankId, deviceUrl, mockResponse = "" }) {
  const endpoint = new URL(PPOINTS_BASE_URL);
  endpoint.searchParams.set("stn_id", stationId);
  endpoint.searchParams.set("bank_id", bankId);
  endpoint.searchParams.set("flg", "W");

  let responseText = "";
  if (mockResponse) {
    responseText = mockResponse;
  } else {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 8000);
    try {
      const response = await fetch(endpoint, { signal: controller.signal });
      responseText = await response.text();
      if (!response.ok) {
        throw new Error(`P-Points responded with HTTP ${response.status}`);
      }
    } catch (error) {
      throw new Error(error.name === "AbortError" ? "P-Points request timed out" : error.message);
    } finally {
      clearTimeout(timeout);
    }
  }

  const parsed = parsePpointsResponse(responseText);
  if (!parsed.ok || !parsed.amount) {
    return {
      ok: false,
      source: "p-points",
      endpoint: mockResponse ? "mock:p-points" : endpoint.href,
      parsed,
      note: "No unread paid amount returned from P-Points.",
    };
  }

  const paymentId = `ppoints-${stationId}-${bankId}-${parsed.count || "0"}-${parsed.amount}`;
  const existing = payments[paymentId];
  if (existing?.deviceNotified) {
    return {
      ok: true,
      duplicate: true,
      payment: paymentPublicView(existing),
      parsed,
      note: "This P-Points amount/count was already sent to ESP32.",
    };
  }

  const parsedDevice = new URL(deviceUrl || DEFAULT_DEVICE_URL);
  if (!isAllowedTarget(parsedDevice)) {
    throw new Error("Device URL must be localhost or a private LAN IP");
  }

  let payment = {
    chargeId: paymentId,
    status: "successful",
    paid: true,
    amountMinor: Math.round(Number(parsed.amount) * 100),
    amount: parsed.amount,
    currency: "THB",
    reference: `PPOINTS-${parsed.count || Date.now()}`,
    qrImageUrl: "",
    livemode: true,
    createdAt: new Date().toISOString(),
    paidAt: new Date().toISOString(),
    expiresAt: "",
    deviceUrl: parsedDevice.href,
    deviceNotified: false,
    source: "p-points",
    ppoints: parsed,
    updatedAt: new Date().toISOString(),
  };
  payments[payment.chargeId] = payment;
  savePayments();

  payment = await notifyDeviceOfPayment(payment);
  return {
    ok: true,
    duplicate: false,
    source: mockResponse ? "p-points-mock" : "p-points",
    payment: paymentPublicView(payment),
    parsed,
  };
}

async function checkPpointsPayment(req, res) {
  try {
    const data = req.method === "POST" ? await readRequestData(req) : Object.fromEntries(new URL(req.url, "http://local").searchParams);
    const stationId = validatePpointsId(data.stn_id || data.stationId || dashboardConfig.ppointsStationId, "P-Points stn_id");
    const bankId = validatePpointsId(data.bank_id || data.bankId || dashboardConfig.ppointsBankId, "P-Points bank_id");
    const deviceUrl = data.deviceUrl || DEFAULT_DEVICE_URL;
    const result = await checkPpointsPayin({ stationId, bankId, deviceUrl });
    sendJson(res, 200, result);
  } catch (error) {
    sendJson(res, 400, { error: error.message });
  }
}

async function mockPpointsPayment(req, res) {
  try {
    const data = req.method === "POST" ? await readRequestData(req) : Object.fromEntries(new URL(req.url, "http://local").searchParams);
    const stationId = validatePpointsId(data.stn_id || data.stationId || dashboardConfig.ppointsStationId, "P-Points stn_id");
    const bankId = validatePpointsId(data.bank_id || data.bankId || dashboardConfig.ppointsBankId, "P-Points bank_id");
    const deviceUrl = data.deviceUrl || DEFAULT_DEVICE_URL;
    const amount = formatMoney(data.amount || "13.03") || "13.03";
    const count = String(data.count || "8");
    const mockResponse = JSON.stringify({ RES: "OK", stn_id: stationId, bank_id: bankId, amt: [amount, count] });
    const result = await checkPpointsPayin({ stationId, bankId, deviceUrl, mockResponse });
    sendJson(res, 200, result);
  } catch (error) {
    sendJson(res, 400, { error: error.message });
  }
}

async function createOmisePayment(req, res) {
  try {
    const data = await readRequestData(req);
    const amountMinor = parseOmiseAmount(data.amount);
    const reference = String(data.reference || `ORDER-${Date.now()}`).trim().slice(0, 50);
    if (!reference) throw new Error("Payment reference is required");

    let deviceUrl = data.deviceUrl || DEFAULT_DEVICE_URL;
    const parsedDevice = new URL(deviceUrl);
    if (!isAllowedTarget(parsedDevice)) throw new Error("Device URL must be localhost or a private LAN IP");
    deviceUrl = parsedDevice.href;

    const charge = await omise.createPromptPayCharge({ amountMinor, reference });
    if (!charge.qrImageUrl) throw new Error("Omise charge did not return a PromptPay QR image");
    const payment = updatePayment(charge, { deviceUrl, deviceNotified: false });
    sendJson(res, 201, paymentPublicView(payment));
  } catch (error) {
    sendJson(res, error.status || 400, { error: error.message });
  }
}

async function refreshOmisePayment(chargeId, res) {
  try {
    const charge = await omise.retrieveCharge(chargeId);
    let payment = updatePayment(charge, payments[chargeId] || {});
    payment = await notifyDeviceOfPayment(payment);
    sendJson(res, 200, paymentPublicView(payment));
  } catch (error) {
    sendJson(res, error.status || 400, { error: error.message });
  }
}

async function receiveOmiseWebhook(req, res) {
  try {
    const event = await readRequestData(req);
    if (event.key !== "charge.complete" || !event.data?.id) {
      sendJson(res, 200, { ok: true, ignored: true });
      return;
    }

    const charge = await omise.retrieveCharge(event.data.id);
    let payment = updatePayment(charge, payments[charge.chargeId] || {});
    payment = await notifyDeviceOfPayment(payment);
    sendJson(res, 200, { ok: true, payment: paymentPublicView(payment) });
  } catch (error) {
    sendJson(res, error.status || 400, { error: error.message });
  }
}

async function localQrSvg(req, res, requestUrl) {
  try {
    const data = requestUrl.searchParams.get("data") || "";
    if (!data) {
      sendJson(res, 400, { error: "data is required" });
      return;
    }
    const svg = await QRCode.toString(data, {
      type: "svg",
      errorCorrectionLevel: "L",
      margin: 4,
      width: 420,
    });
    send(res, 200, svg, { "Content-Type": "image/svg+xml" });
  } catch (error) {
    sendJson(res, 400, { error: error.message });
  }
}

function localQrEvent(res, requestUrl, dynamic) {
  try {
    const event = buildPromptPayPayload({
      promptpay: requestUrl.searchParams.get("promptpay") || dashboardConfig.promptPayId,
      amount: dynamic ? requestUrl.searchParams.get("amount") || "0" : "0",
      ref: requestUrl.searchParams.get("ref") || (dynamic ? "DYNAMIC" : "STATIC"),
      dynamic,
    });
    sendJson(res, 200, event);
  } catch (error) {
    sendJson(res, 400, { error: error.message });
  }
}

async function proxy(req, res, requestUrl) {
  let target;
  try {
    target = new URL(requestUrl.searchParams.get("target") || DEFAULT_DEVICE_URL);
  } catch {
    sendJson(res, 400, { error: "Invalid target URL" });
    return;
  }

  if (!isAllowedTarget(target)) {
    sendJson(res, 400, { error: "Target must be localhost or a private LAN IP" });
    return;
  }

  const path = requestUrl.pathname.replace(/^\/proxy/, "");
  const targetUrl = new URL(path || "/", target);
  for (const [key, value] of requestUrl.searchParams.entries()) {
    if (key !== "target") {
      targetUrl.searchParams.append(key, value);
    }
  }

  const body = ["POST", "PUT", "PATCH"].includes(req.method) ? await readBody(req) : undefined;
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 8000);

  try {
    const response = await fetch(targetUrl, {
      method: req.method,
      body,
      signal: controller.signal,
      headers: {
        "Content-Type": req.headers["content-type"] || "application/x-www-form-urlencoded",
      },
    });

    const bytes = Buffer.from(await response.arrayBuffer());
    send(res, response.status, bytes, {
      "Content-Type": response.headers.get("content-type") || "application/octet-stream",
    });
  } catch (error) {
    sendJson(res, 502, {
      error: error.name === "AbortError" ? "Device request timed out" : "Cannot reach ESP32 device",
      target: target.href,
    });
  } finally {
    clearTimeout(timeout);
  }
}

function pageHtml() {
  return `<!doctype html>
<html lang="th">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>PromptPay ESP32 Control</title>
  <style>
    :root { color-scheme: light; --line:#d8dee8; --ink:#172033; --muted:#5b6472; --accent:#0f766e; --dark:#263244; --danger:#b42318; }
    * { box-sizing: border-box; }
    body { margin: 0; font-family: Arial, sans-serif; background: #f5f7fa; color: var(--ink); }
    header { padding: 18px 24px; border-bottom: 1px solid var(--line); background: #fff; display: flex; justify-content: space-between; gap: 16px; align-items: center; }
    h1 { font-size: 20px; margin: 0; }
    main { max-width: 1100px; margin: 0 auto; padding: 20px; display: grid; grid-template-columns: minmax(300px, 420px) 1fr; gap: 18px; }
    section { background: #fff; border: 1px solid var(--line); border-radius: 8px; padding: 16px; }
    h2 { font-size: 16px; margin: 0 0 12px; }
    label { display: block; font-size: 13px; color: var(--muted); margin: 12px 0 5px; }
    input { width: 100%; border: 1px solid var(--line); border-radius: 6px; padding: 11px 12px; font: inherit; }
    input[type="checkbox"] { width: auto; }
    button { border: 0; border-radius: 6px; padding: 11px 12px; font: inherit; font-weight: 700; color: #fff; background: var(--accent); cursor: pointer; }
    button.secondary { background: var(--dark); }
    button.ghost { color: var(--dark); background: #eef2f7; }
    .row { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-top: 14px; }
    .status { font-size: 13px; color: var(--muted); }
    .status strong { color: var(--ink); }
    .checkrow { display: flex; align-items: center; gap: 8px; margin-top: 12px; color: var(--muted); font-size: 13px; }
    .qrBox { min-height: 420px; display: flex; align-items: center; justify-content: center; background: #f9fafb; border: 1px solid var(--line); border-radius: 8px; }
    #qr { width: min(380px, 88vw); height: min(380px, 88vw); background: #fff; border: 12px solid #fff; box-shadow: 0 8px 28px #15233a24; }
    pre { white-space: pre-wrap; word-break: break-word; background: #f2f5f9; border: 1px solid var(--line); border-radius: 8px; padding: 12px; min-height: 140px; max-height: 340px; overflow: auto; font-size: 12px; }
    table { width: 100%; border-collapse: collapse; font-size: 13px; }
    th, td { text-align: left; border-bottom: 1px solid var(--line); padding: 8px 6px; vertical-align: top; }
    th { color: var(--muted); font-weight: 700; }
    .error { color: var(--danger); font-weight: 700; }
    @media (max-width: 860px) { main { grid-template-columns: 1fr; padding: 14px; } header { display: block; } .row { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <header>
    <h1>PromptPay ESP32 Control</h1>
    <div class="status">Device: <strong id="deviceLabel">-</strong></div>
  </header>

  <main>
    <section>
      <h2>Generate PromptPay QR</h2>
      <label for="target">ESP32 / Wokwi URL</label>
      <input id="target" value="${DEFAULT_DEVICE_URL}">
      <div class="status">This is the device that shows OLED and pulse output, for example http://localhost:8180 or http://paymentesp.local. P-Points is configured below.</div>
      <label class="checkrow"><input id="syncDevice" type="checkbox"> Sync to ESP32 hardware</label>

      <label for="promptpay">PromptPay ID</label>
      <input id="promptpay" inputmode="numeric" value="${dashboardConfig.promptPayId}" placeholder="0812345678">
      <div class="status" id="promptpayStatus">Saved on this machine</div>

      <label for="webhook">REST POST Webhook URL</label>
      <input id="webhook" value="${htmlEscape(dashboardConfig.webhookUrl)}" placeholder="http://server/api/payment">
      <div class="status">Optional: use the dashboard computer's LAN IP, for example http://192.168.1.10:3001/api/webhook</div>

      <label for="amount">Amount (THB)</label>
      <input id="amount" type="number" min="0.01" step="0.01" inputmode="decimal" value="99.00" placeholder="99.00" required>
      <div class="status">Dynamic QR uses this amount. Static QR has no amount.</div>

      <label for="ref">Reference</label>
      <input id="ref" value="ORDER-0001">

      <div class="row">
        <button id="dynamicBtn">Dynamic QR with Amount</button>
        <button id="staticBtn" class="secondary">Static QR no Amount</button>
      </div>
      <div class="row">
        <button id="saveBtn" class="ghost">Save Settings</button>
        <button id="logsBtn" class="ghost">Refresh Device Logs</button>
      </div>
      <div class="row">
        <button id="webhookLogsBtn" class="ghost">Refresh REST POST Logs</button>
      </div>
    </section>

    <section>
      <h2>P-Points Pay-in</h2>
      <label for="ppointsStation">stn_id</label>
      <input id="ppointsStation" value="${htmlEscape(dashboardConfig.ppointsStationId)}" placeholder="P-24001">
      <label for="ppointsBank">bank_id</label>
      <input id="ppointsBank" value="${htmlEscape(dashboardConfig.ppointsBankId)}" placeholder="X-9786">
      <div class="status">Checks unread pay-in amount from P-Points with flg=W, then sends paid status to ESP32.</div>
      <div class="row">
        <button id="ppointsFullTestBtn">Test Full P-Points Flow</button>
      </div>
      <div class="row">
        <button id="ppointsCheckBtn">Check P-Points Pay-in</button>
        <button id="ppointsSaveBtn" class="ghost">Save P-Points IDs</button>
      </div>
      <div class="row">
        <button id="ppointsMockBtn" class="secondary">Mock P-Points 13.03</button>
        <button id="customerQrBtn" class="ghost">Show Customer QR on ESP32</button>
      </div>
    </section>

    <section>
      <div class="qrBox"><img id="qr" alt="PromptPay QR"></div>
    </section>

    <section>
      <h2 style="font-size:16px;margin:0 0 10px">Response</h2>
      <pre id="output">Ready</pre>
    </section>

    <section>
      <h2 style="font-size:16px;margin:0 0 10px">Logs</h2>
      <div id="logs"></div>
    </section>

    <section>
      <h2 style="font-size:16px;margin:0 0 10px">REST POST Events</h2>
      <div id="webhookLogs"></div>
    </section>

  </main>

  <script>
    const targetInput = document.getElementById('target');
    const promptpayInput = document.getElementById('promptpay');
    const webhookInput = document.getElementById('webhook');
    const amountInput = document.getElementById('amount');
    const refInput = document.getElementById('ref');
    const ppointsStationInput = document.getElementById('ppointsStation');
    const ppointsBankInput = document.getElementById('ppointsBank');
    const output = document.getElementById('output');
    const qr = document.getElementById('qr');
    const logs = document.getElementById('logs');
    const webhookLogs = document.getElementById('webhookLogs');
    const deviceLabel = document.getElementById('deviceLabel');
    const syncDevice = document.getElementById('syncDevice');
    let latestQrPayload = '';

    targetInput.value = localStorage.getItem('esp32Target') || targetInput.value;
    amountInput.value = localStorage.getItem('promptpayAmount') || amountInput.value;
    refInput.value = localStorage.getItem('promptpayReference') || refInput.value;
    syncDevice.checked = localStorage.getItem('syncDevice') === 'true';

    function targetParam() {
      const target = targetInput.value.trim();
      localStorage.setItem('esp32Target', target);
      deviceLabel.textContent = target;
      return encodeURIComponent(target);
    }

    function show(data) {
      output.textContent = JSON.stringify(data, null, 2);
      output.classList.toggle('error', Boolean(data && data.error));
    }

    function escapeHtml(value) {
      return String(value == null ? '' : value).replace(/[&<>"']/g, char => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
      })[char]);
    }

    async function requestJson(path, options) {
      const response = await fetch('/proxy' + path + (path.includes('?') ? '&' : '?') + 'target=' + targetParam(), options);
      const text = await response.text();
      try { return JSON.parse(text); } catch { return { raw: text, status: response.status }; }
    }

    async function localJson(path, options) {
      const response = await fetch('/local' + path, options);
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || 'Local dashboard request failed');
      return data;
    }

    async function loadConfig() {
      const local = await localJson('/config');
      if (local.promptPayId) promptpayInput.value = local.promptPayId;
      webhookInput.value = local.webhookUrl || '';
      ppointsStationInput.value = local.ppointsStationId || ppointsStationInput.value;
      ppointsBankInput.value = local.ppointsBankId || ppointsBankInput.value;
      document.getElementById('promptpayStatus').textContent = 'Saved on this machine: ' + local.promptPayId;
      deviceLabel.textContent = targetInput.value.trim();
      return local;
    }

    async function saveConfig() {
      const promptPayId = promptpayInput.value.trim();
      const webhookUrl = webhookInput.value.trim();
      const local = await localJson('/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          promptpay: promptPayId,
          webhook: webhookUrl,
          ppointsStationId: ppointsStationInput.value.trim(),
          ppointsBankId: ppointsBankInput.value.trim()
        })
      });
      promptpayInput.value = local.promptPayId;
      webhookInput.value = local.webhookUrl || '';
      document.getElementById('promptpayStatus').textContent = 'Saved on this machine: ' + local.promptPayId;
      if (!syncDevice.checked) {
        show({ ok: true, saved: local, note: 'Saved permanently on this machine. ESP32 sync is off.' });
        return;
      }
      const device = await requestJson('/api/config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({ promptpay: local.promptPayId, webhook: local.webhookUrl || '' })
      });
      show({ ok: !device.error, saved: local, device, note: device.error ? 'Saved locally, but ESP32 sync failed.' : 'Saved locally and synced to ESP32 hardware.' });
    }

    async function checkPpoints() {
      const response = await fetch('/api/ppoints/check', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          stn_id: ppointsStationInput.value.trim(),
          bank_id: ppointsBankInput.value.trim(),
          deviceUrl: targetInput.value.trim()
        })
      });
      const data = await response.json();
      show(data);
      await loadWebhookLogs();
      return data;
    }

    async function mockPpoints() {
      const response = await fetch('/api/ppoints/mock', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams({
          stn_id: ppointsStationInput.value.trim(),
          bank_id: ppointsBankInput.value.trim(),
          amount: '13.03',
          count: String(Date.now()),
          deviceUrl: targetInput.value.trim()
        })
      });
      const data = await response.json();
      show(data);
      await loadWebhookLogs();
      return data;
    }

    async function showCustomerQrOnDevice() {
      if (!latestQrPayload) {
        await createQr('dynamic');
      }
      const params = new URLSearchParams({
        bank_id: ppointsBankInput.value.trim(),
        stn_id: ppointsStationInput.value.trim(),
        ref: 'PPOINTS-QR',
        payload: latestQrPayload
      });
      const device = await requestJson('/api/customer-qr?' + params.toString());
      show({
        device,
        note: device.error
          ? 'ESP32/Wokwi is not reachable. Start Wokwi or set the correct ESP32 Hardware URL.'
          : 'Customer/P-Points QR mode displayed on ESP32 OLED.'
      });
      return device;
    }

    async function testFullPpointsFlow() {
      const qrData = await createQr('dynamic');
      const customerQr = await showCustomerQrOnDevice();
      const payment = await mockPpoints();
      const paymentReachedDevice = Boolean(payment.payment && payment.payment.deviceNotified);
      show({
        ok: !customerQr.error && paymentReachedDevice,
        step1QrOnWeb: qrData,
        step2QrOnDevice: customerQr,
        step3MockPayin: payment,
        note: customerQr.error
          ? 'Dashboard created the QR, but Wokwi/ESP32 is not reachable. Start Wokwi and set ESP32 / Wokwi URL correctly.'
          : !paymentReachedDevice
            ? 'Dashboard read the mock P-Points amount, but ESP32/Wokwi did not confirm payment notification. Check ESP32 / Wokwi URL.'
          : 'Full P-Points mock flow completed. OLED should show PAID and LED should pulse.'
      });
    }

    async function createQr(mode) {
      const params = new URLSearchParams({
        promptpay: promptpayInput.value.trim(),
        ref: refInput.value.trim()
      });
      if (mode === 'dynamic') params.set('amount', amountInput.value.trim());

      const local = await localJson('/' + mode + '?' + params.toString());
      show(local);
      if (local.payload) {
        latestQrPayload = local.payload;
        qr.src = '/local/qr.svg?data=' + encodeURIComponent(local.payload) + '&t=' + Date.now();
      }

      if (!syncDevice.checked) {
        const result = {
          localQr: local,
          note: mode === 'static'
            ? 'Static QR generated. Static PromptPay QR does not include amount. Use Dynamic QR with Amount if you need amount.'
            : 'Dynamic QR generated with amount. ESP32 sync is off.'
        };
        show(result);
        return result;
      }

      const device = await requestJson('/api/' + mode + '?' + params.toString());
      if (device.error) {
        const result = { localQr: local, device: device, note: 'Local QR generated. ESP32 sync failed because the device is not responding yet.' };
        show(result);
        return result;
      }

      const result = { localQr: local, device };
      show(result);
      await loadLogs(false);
      await loadWebhookLogs();
      return result;
    }

    async function loadLogs(printResponse = true) {
      if (!syncDevice.checked) {
        const data = { source: 'local-dashboard', logs: [], note: 'Device logs require ESP32 sync to be enabled.' };
        if (printResponse) show(data);
        logs.innerHTML = '<pre>' + JSON.stringify(data, null, 2) + '</pre>';
        return;
      }
      const data = await requestJson('/api/logs');
      if (printResponse) show(data);
      if (!Array.isArray(data)) {
        logs.innerHTML = '<pre>' + JSON.stringify(data, null, 2) + '</pre>';
        return;
      }
      logs.innerHTML = '<table><thead><tr><th>Mode</th><th>Amount</th><th>Ref</th><th>Time</th><th>POST</th></tr></thead><tbody>' +
        data.slice().reverse().map(item => '<tr><td>' + escapeHtml(item.mode) + '</td><td>' + escapeHtml(item.amount || '-') + '</td><td>' + escapeHtml(item.reference) + '</td><td>' + escapeHtml(item.createdAt || item.createdAtMs) + '</td><td>' + escapeHtml(item.webhookStatus || '-') + '</td></tr>').join('') +
        '</tbody></table>';
    }

    async function loadWebhookLogs() {
      const response = await fetch('/api/webhook/logs');
      const data = await response.json();
      if (!Array.isArray(data) || data.length === 0) {
        webhookLogs.innerHTML = '<pre>No REST POST events received yet.</pre>';
        return;
      }
      webhookLogs.innerHTML = '<table><thead><tr><th>Mode</th><th>Amount</th><th>Ref</th><th>Created</th><th>Received</th></tr></thead><tbody>' +
        data.slice().reverse().map(item => '<tr><td>' + escapeHtml(item.mode) + '</td><td>' + escapeHtml(item.amount || '-') + '</td><td>' + escapeHtml(item.reference) + '</td><td>' + escapeHtml(item.createdAt || '-') + '</td><td>' + escapeHtml(item.receivedAt) + '</td></tr>').join('') +
        '</tbody></table>';
    }

    document.getElementById('dynamicBtn').onclick = () => createQr('dynamic').catch((error) => show({ error: error.message }));
    document.getElementById('staticBtn').onclick = () => createQr('static').catch((error) => show({ error: error.message }));
    document.getElementById('saveBtn').onclick = () => saveConfig().catch((error) => show({ error: error.message }));
    document.getElementById('logsBtn').onclick = () => loadLogs(true).catch((error) => show({ error: error.message }));
    document.getElementById('webhookLogsBtn').onclick = () => loadWebhookLogs().catch((error) => show({ error: error.message }));
    document.getElementById('ppointsFullTestBtn').onclick = () => testFullPpointsFlow().catch((error) => show({ error: error.message }));
    document.getElementById('ppointsCheckBtn').onclick = () => checkPpoints().catch((error) => show({ error: error.message }));
    document.getElementById('ppointsMockBtn').onclick = () => mockPpoints().catch((error) => show({ error: error.message }));
    document.getElementById('customerQrBtn').onclick = () => showCustomerQrOnDevice().catch((error) => show({ error: error.message }));
    document.getElementById('ppointsSaveBtn').onclick = () => saveConfig().catch((error) => show({ error: error.message }));
    targetInput.onchange = () => loadConfig().catch((error) => show({ error: error.message }));
    amountInput.oninput = () => localStorage.setItem('promptpayAmount', amountInput.value);
    refInput.oninput = () => localStorage.setItem('promptpayReference', refInput.value);
    amountInput.onkeydown = (event) => {
      if (event.key === 'Enter') {
        event.preventDefault();
        createQr('dynamic').catch((error) => show({ error: error.message }));
      }
    };
    syncDevice.onchange = () => {
      localStorage.setItem('syncDevice', syncDevice.checked ? 'true' : 'false');
      if (syncDevice.checked) {
        saveConfig().catch((error) => show({ error: error.message }));
      }
    };
    Promise.all([loadConfig(), loadWebhookLogs()])
      .then(() => createQr('dynamic'))
      .catch((error) => show({ error: error.message }));
  </script>
</body>
</html>`;
}

const server = http.createServer(async (req, res) => {
  const requestUrl = new URL(req.url, `http://${req.headers.host}`);

  if (requestUrl.pathname === "/") {
    send(res, 200, pageHtml(), { "Content-Type": "text/html; charset=utf-8" });
    return;
  }

  if (requestUrl.pathname === "/evidence" && req.method === "GET") {
    if (!fs.existsSync(EVIDENCE_FILE)) {
      sendJson(res, 404, { error: "Evidence report has not been generated" });
      return;
    }
    send(res, 200, fs.readFileSync(EVIDENCE_FILE), { "Content-Type": "text/html; charset=utf-8" });
    return;
  }

  if (requestUrl.pathname === "/api/omise/config" && req.method === "GET") {
    sendJson(res, 200, {
      configured: omise.configured,
      mode: omise.mode,
      publicKeyConfigured: Boolean(process.env.OMISE_PUBLIC_KEY),
      webhookUrl: PUBLIC_BASE_URL ? PUBLIC_BASE_URL + "/api/omise/webhook" : "",
      minimumAmount: "20.00",
      maximumAmount: "150000.00",
    });
    return;
  }

  if (requestUrl.pathname === "/api/omise/charges" && req.method === "POST") {
    await createOmisePayment(req, res);
    return;
  }

  const omiseChargeMatch = requestUrl.pathname.match(/^\/api\/omise\/charges\/(chrg(?:_test)?_[0-9a-z]+)$/);
  if (omiseChargeMatch && req.method === "GET") {
    await refreshOmisePayment(omiseChargeMatch[1], res);
    return;
  }

  if (requestUrl.pathname === "/api/omise/payments" && req.method === "GET") {
    const list = Object.values(payments)
      .sort((a, b) => String(b.updatedAt).localeCompare(String(a.updatedAt)))
      .map(paymentPublicView);
    sendJson(res, 200, list);
    return;
  }

  if (requestUrl.pathname === "/api/omise/webhook" && req.method === "POST") {
    await receiveOmiseWebhook(req, res);
    return;
  }

  if (requestUrl.pathname === "/api/ppoints/check" && ["GET", "POST"].includes(req.method)) {
    await checkPpointsPayment(req, res);
    return;
  }

  if (requestUrl.pathname === "/api/ppoints/mock" && ["GET", "POST"].includes(req.method)) {
    await mockPpointsPayment(req, res);
    return;
  }

  if (requestUrl.pathname === "/api/webhook" && req.method === "POST") {
    await receiveWebhook(req, res);
    return;
  }

  if (requestUrl.pathname === "/api/webhook/logs" && req.method === "GET") {
    sendJson(res, 200, webhookEvents);
    return;
  }

  if (requestUrl.pathname === "/local/dynamic") {
    localQrEvent(res, requestUrl, true);
    return;
  }

  if (requestUrl.pathname === "/local/config") {
    try {
      if (req.method === "POST") {
        const body = new URLSearchParams((await readBody(req)).toString("utf8"));
        dashboardConfig = saveDashboardConfig({
          promptPayId: body.get("promptpay") || "",
          webhookUrl: body.has("webhook") ? body.get("webhook") : dashboardConfig.webhookUrl,
          ppointsStationId: body.get("ppointsStationId") || body.get("stn_id") || dashboardConfig.ppointsStationId,
          ppointsBankId: body.get("ppointsBankId") || body.get("bank_id") || dashboardConfig.ppointsBankId,
        });
      }
      sendJson(res, 200, { ...dashboardConfig, persisted: true });
    } catch (error) {
      sendJson(res, 400, { error: error.message });
    }
    return;
  }

  if (requestUrl.pathname === "/local/static") {
    localQrEvent(res, requestUrl, false);
    return;
  }

  if (requestUrl.pathname === "/local/qr.svg") {
    await localQrSvg(req, res, requestUrl);
    return;
  }

  if (requestUrl.pathname.startsWith("/proxy/")) {
    await proxy(req, res, requestUrl);
    return;
  }

  sendJson(res, 404, { error: "Not found" });
});

server.listen(PORT, () => {
  console.log(`PromptPay dashboard: http://localhost:${PORT}`);
  console.log(`Default ESP32 target: ${DEFAULT_DEVICE_URL}`);
});
