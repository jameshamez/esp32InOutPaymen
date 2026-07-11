const API_BASE_URL = "https://api.omise.co";

function normalizeCharge(charge) {
  const amountMinor = Number(charge.amount || 0);
  const reference = charge.metadata?.reference || charge.description || "";
  const qrImageUrl = charge.source?.scannable_code?.image?.download_uri || "";

  return {
    chargeId: charge.id,
    status: charge.status,
    paid: charge.status === "successful" || charge.paid === true,
    amountMinor,
    amount: (amountMinor / 100).toFixed(2),
    currency: String(charge.currency || "THB").toUpperCase(),
    reference,
    qrImageUrl,
    livemode: Boolean(charge.livemode),
    createdAt: charge.created_at || "",
    paidAt: charge.paid_at || "",
    expiresAt: charge.expires_at || "",
  };
}

class OmiseClient {
  constructor({ secretKey = "", fetchImpl = globalThis.fetch } = {}) {
    this.secretKey = secretKey;
    this.fetchImpl = fetchImpl;
  }

  get configured() {
    return Boolean(this.secretKey);
  }

  get mode() {
    if (!this.secretKey) return "unconfigured";
    return this.secretKey.startsWith("skey_test_") ? "test" : "live";
  }

  async request(method, pathname, form) {
    if (!this.configured) {
      throw new Error("OMISE_SECRET_KEY is not configured");
    }

    const response = await this.fetchImpl(API_BASE_URL + pathname, {
      method,
      headers: {
        Authorization: `Basic ${Buffer.from(`${this.secretKey}:`).toString("base64")}`,
        ...(form ? { "Content-Type": "application/x-www-form-urlencoded" } : {}),
      },
      body: form ? new URLSearchParams(form) : undefined,
    });
    const data = await response.json();
    if (!response.ok) {
      const message = data.message || data.code || `Omise API returned HTTP ${response.status}`;
      const error = new Error(message);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  async createPromptPayCharge({ amountMinor, reference }) {
    const charge = await this.request("POST", "/charges", {
      amount: String(amountMinor),
      currency: "THB",
      description: reference,
      "metadata[reference]": reference,
      "source[type]": "promptpay",
    });
    return normalizeCharge(charge);
  }

  async retrieveCharge(chargeId) {
    if (!/^chrg(_test)?_[0-9a-z]+$/.test(chargeId)) {
      throw new Error("Invalid Omise charge ID");
    }
    return normalizeCharge(await this.request("GET", `/charges/${chargeId}`));
  }
}

module.exports = { OmiseClient, normalizeCharge };
