const assert = require("assert");
const { OmiseClient, normalizeCharge } = require("../local-dashboard/omise");

const chargeResponse = {
  id: "chrg_test_123abc",
  status: "pending",
  amount: 2000,
  currency: "THB",
  livemode: false,
  description: "ORDER-OMISE",
  metadata: { reference: "ORDER-OMISE" },
  source: {
    scannable_code: {
      image: { download_uri: "https://api.omise.co/qr.svg" },
    },
  },
  created_at: "2026-06-27T00:00:00Z",
};

async function main() {
  const normalized = normalizeCharge(chargeResponse);
  assert.equal(normalized.amount, "20.00");
  assert.equal(normalized.reference, "ORDER-OMISE");
  assert.equal(normalized.qrImageUrl, "https://api.omise.co/qr.svg");
  assert.equal(normalized.paid, false);

  let captured;
  const client = new OmiseClient({
    secretKey: "skey_test_unit_fake",
    fetchImpl: async (url, options) => {
      captured = { url, options };
      return { ok: true, status: 200, json: async () => chargeResponse };
    },
  });
  const created = await client.createPromptPayCharge({ amountMinor: 2000, reference: "ORDER-OMISE" });
  assert.equal(client.mode, "test");
  assert.equal(created.chargeId, "chrg_test_123abc");
  assert.equal(captured.url, "https://api.omise.co/charges");
  assert.equal(captured.options.method, "POST");
  assert.match(captured.options.body.toString(), /source%5Btype%5D=promptpay/);
  assert.match(captured.options.body.toString(), /amount=2000/);
  assert.ok(!captured.options.headers.Authorization.includes("skey_test_unit_fake"));

  await client.retrieveCharge("chrg_test_123abc");
  assert.equal(captured.options.method, "GET");
  assert.equal(captured.url, "https://api.omise.co/charges/chrg_test_123abc");

  await assert.rejects(() => client.retrieveCharge("../../secret"), /Invalid Omise charge ID/);
  console.log("Omise integration tests passed");
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
