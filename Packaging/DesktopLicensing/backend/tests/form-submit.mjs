import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import { webcrypto } from "node:crypto";
import { fileURLToPath } from "node:url";

const source = fs.readFileSync(
  fileURLToPath(new URL("../public/account-assets/app.js", import.meta.url)),
  "utf8"
);

const values = {
  display_name: "Cliente Teste",
  email: "checkout-regression@example.invalid",
  password: "Senha de teste 123!",
  plan_code: "complete",
  accept_terms: "on",
};

const controls = Object.entries(values).map(([name, value]) => ({
  checked: name === "accept_terms" || name === "plan_code",
  disabled: false,
  name,
  type: name === "accept_terms" ? "checkbox" : name === "plan_code" ? "radio" : "text",
  value,
}));

let submitHandler = null;
const requests = [];
const storage = new Map();
const statusNodes = new Map();
const timers = [];
let checkoutStatus = {
  paid: false,
  license_issued: false,
  status: "pending",
  message: "Aguardando",
};
const form = {
  elements: controls,
  hidden: false,
  reportValidity: () => true,
  querySelectorAll: () => [],
  addEventListener: (type, callback) => {
    if (type === "submit") submitHandler = callback;
  },
};
const payment = { hidden: true };
const qr = { hidden: true, src: "" };
const code = { value: "" };
const open = { hidden: true, href: "" };
const live = { textContent: "", classList: { add: () => {} } };

class FakeFormData {
  constructor(target) {
    this.entries = new Map();
    for (const control of target.elements) {
      if (control.disabled || !control.name) continue;
      if (["checkbox", "radio"].includes(control.type) && !control.checked) continue;
      this.entries.set(control.name, control.value);
    }
  }

  get(name) {
    return this.entries.get(name) ?? null;
  }
}

const document = {
  body: { dataset: { page: "purchase" } },
  getElementById: id => {
    if (id === "purchase-form") return form;
    if (id === "pix-payment") return payment;
    if (id === "pix-qr") return qr;
    if (id === "pix-code") return code;
    if (id === "pix-open") return open;
    if (id === "pix-live") return live;
    if (id === "form-status" || id === "pix-status") {
      if (!statusNodes.has(id)) statusNodes.set(id, { textContent: "" });
      return statusNodes.get(id);
    }
    return null;
  },
};

const sessionStorage = {
  getItem: key => storage.get(key) ?? null,
  removeItem: key => storage.delete(key),
  setItem: (key, value) => storage.set(key, String(value)),
};

const location = {
  pathname: "/comprar",
  hash: "",
  assign: () => {},
};

const context = {
  URLSearchParams,
  Uint8Array,
  crypto: webcrypto,
  document,
  FormData: FakeFormData,
  history: { replaceState: () => {} },
  navigator: { clipboard: { writeText: async () => {} } },
  sessionStorage,
  window: {
    clearTimeout: () => {},
    location,
    setTimeout: callback => {
      timers.push(callback);
      return timers.length;
    },
  },
  fetch: async (url, options) => {
    requests.push({ url, options });
    const payload = url === "/api/v1/checkout/start"
      ? {
          order_id: "order-test",
          checkout_token: "checkout-test",
          checkout_url: "https://example.invalid/pix",
          pix_copy_paste: "PIX-TEST",
          qr_code_image: "",
        }
      : checkoutStatus;
    return {
      ok: true,
      status: 200,
      json: async () => payload,
    };
  },
  setTimeout: () => 1,
};

vm.runInNewContext(source, context, { filename: "app.js" });
assert.equal(typeof submitHandler, "function");
const firstCheckoutKey = storage.get("churchdaw_checkout_idempotency");
await submitHandler({ preventDefault: () => {} });
await new Promise(resolve => setImmediate(resolve));

assert.equal(requests[0].url, "/api/v1/checkout/start");
assert.deepEqual(
  JSON.parse(requests[0].options.body),
  {
    display_name: values.display_name,
    email: values.email,
    password: values.password,
    plan_code: values.plan_code,
    accept_terms: true,
  }
);
assert.ok(controls.every(control => control.disabled));
assert.equal(form.hidden, true);
assert.equal(payment.hidden, false);
assert.equal(code.value, "PIX-TEST");

checkoutStatus = {
  paid: false,
  license_issued: false,
  status: "expired",
  message: "Este Pix expirou. Inicie uma nova compra.",
};
assert.ok(timers.length >= 1);
await timers.shift()();

assert.equal(storage.has("churchdaw_pix_payment"), false);
assert.notEqual(storage.get("churchdaw_checkout_idempotency"), firstCheckoutKey);
assert.equal(form.hidden, false);
assert.equal(payment.hidden, true);
assert.ok(controls.every(control => !control.disabled));
assert.equal(
  statusNodes.get("form-status").textContent,
  "Este Pix expirou. Inicie uma nova compra."
);

console.log("PASS: compra captura campos e Pix expirado gera uma nova tentativa limpa");
