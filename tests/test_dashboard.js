const assert = require("assert");
const fs = require("fs");
const http = require("http");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");

const port = 32000 + (process.pid % 1000);
const ppointsPort = port + 1000;
const baseUrl = `http://127.0.0.1:${port}`;
const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "paymentesp-dashboard-"));
const configFile = path.join(tempDir, "config.json");
const serverFile = path.join(__dirname, "..", "local-dashboard", "server.js");
const ppointsServer = http.createServer((req, res) => {
  const requestUrl = new URL(req.url, `http://${req.headers.host}`);
  assert.equal(requestUrl.searchParams.get("stn_id"), "S-24001");
  assert.equal(requestUrl.searchParams.get("bank_id"), "X-9786");
  assert.equal(requestUrl.searchParams.get("flg"), "W");
  res.writeHead(200, { "Content-Type": "application/json" });
  res.end(JSON.stringify({ RES: "OK", stn_id: "S-24001", bank_id: "X-9786", amt: ["13.03", "8"] }));
});
const server = spawn(process.execPath, [serverFile], {
  env: {
    ...process.env,
    DASHBOARD_PORT: String(port),
    DASHBOARD_CONFIG_FILE: configFile,
    PAYMENTS_FILE: path.join(tempDir, "payments.json"),
    OMISE_SECRET_KEY: "",
    PPOINTS_BASE_URL: `http://127.0.0.1:${ppointsPort}/sms_payin_rd.php`,
  },
  stdio: ["ignore", "pipe", "pipe"],
});

let serverOutput = "";
server.stdout.on("data", (chunk) => { serverOutput += chunk; });
server.stderr.on("data", (chunk) => { serverOutput += chunk; });

async function waitForServer() {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`${baseUrl}/local/config`);
      if (response.ok) return;
    } catch {
      // Server startup is still in progress.
    }
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error(`Dashboard did not start.\n${serverOutput}`);
}

async function json(pathname, options) {
  const response = await fetch(baseUrl + pathname, options);
  const data = await response.json();
  return { response, data };
}

async function run() {
  await new Promise((resolve) => ppointsServer.listen(ppointsPort, "127.0.0.1", resolve));
  await waitForServer();

  const form = new URLSearchParams({
    promptpay: "0812345678",
    webhook: `http://host.wokwi.internal:${port}/api/webhook`,
    ppointsStationId: "S-24001",
    ppointsBankId: "X-9786",
  });
  const saved = await json("/local/config", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: form,
  });
  assert.equal(saved.response.status, 200);
  assert.equal(saved.data.persisted, true);
  assert.equal(saved.data.promptPayId, "0812345678");
  assert.equal(saved.data.ppointsStationId, "S-24001");
  assert.equal(saved.data.ppointsBankId, "X-9786");
  assert.equal(JSON.parse(fs.readFileSync(configFile, "utf8")).promptPayId, "0812345678");

  const omiseConfig = await json("/api/omise/config");
  assert.equal(omiseConfig.response.status, 200);
  assert.equal(omiseConfig.data.configured, false);

  const omiseWithoutKey = await json("/api/omise/charges", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ amount: "20.00", reference: "OMISE-NO-KEY" }),
  });
  assert.equal(omiseWithoutKey.response.status, 400);
  assert.match(omiseWithoutKey.data.error, /OMISE_SECRET_KEY/);

  const dynamic = await json("/local/dynamic?promptpay=0812345678&amount=15&ref=AUTOMATED");
  assert.equal(dynamic.response.status, 200);
  assert.equal(dynamic.data.amount, "15.00");
  assert.equal(dynamic.data.mode, "dynamic");
  assert.match(dynamic.data.createdAt, /^\d{4}-\d{2}-\d{2}T/);

  const invalidAmount = await json("/local/dynamic?promptpay=0812345678&amount=0&ref=INVALID");
  assert.equal(invalidAmount.response.status, 400);

  const staticQr = await json("/local/static?promptpay=0812345678&amount=999&ref=STATIC");
  assert.equal(staticQr.response.status, 200);
  assert.equal(staticQr.data.amount, "");
  assert.equal(staticQr.data.mode, "static");

  const webhook = await json("/api/webhook", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(dynamic.data),
  });
  assert.equal(webhook.response.status, 202);
  assert.equal(webhook.data.ok, true);

  const webhookLogs = await json("/api/webhook/logs");
  assert.equal(webhookLogs.response.status, 200);
  assert.equal(webhookLogs.data.length, 1);
  assert.equal(webhookLogs.data[0].reference, "AUTOMATED");

  const ppoints = await json("/api/ppoints/check", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      stn_id: "S-24001",
      bank_id: "X-9786",
      deviceUrl: "http://127.0.0.1:9",
    }),
  });
  assert.equal(ppoints.response.status, 200);
  assert.equal(ppoints.data.ok, true);
  assert.equal(ppoints.data.parsed.amount, "13.03");
  assert.equal(ppoints.data.parsed.count, "8");
  assert.equal(ppoints.data.payment.amount, "13.03");

  const ppointsMock = await json("/api/ppoints/mock", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({
      stn_id: "S-24001",
      bank_id: "X-9786",
      amount: "13.03",
      count: "mock-1",
      deviceUrl: "http://127.0.0.1:9",
    }),
  });
  assert.equal(ppointsMock.response.status, 200);
  assert.equal(ppointsMock.data.ok, true);
  assert.equal(ppointsMock.data.source, "p-points-mock");
  assert.equal(ppointsMock.data.parsed.amount, "13.03");

  const invalid = await json("/local/config", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ promptpay: "123", webhook: "not-a-url" }),
  });
  assert.equal(invalid.response.status, 400);

  console.log("Dashboard integration tests passed");
}

run()
  .catch((error) => {
    console.error(error);
    process.exitCode = 1;
  })
  .finally(() => {
    server.kill("SIGTERM");
    ppointsServer.close();
    fs.rmSync(tempDir, { recursive: true, force: true });
  });
