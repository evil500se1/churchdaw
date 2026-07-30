import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";
import { webcrypto } from "node:crypto";
import { fileURLToPath } from "node:url";

const source = fs.readFileSync(
  fileURLToPath(new URL("../public/account-assets/app.js", import.meta.url)),
  "utf8"
);

const loadPage = ({ page, pathname, search = "", hash = "", password = "" }) => {
  let submitHandler = null;
  const requests = [];
  const replacedUrls = [];
  const assignedUrls = [];
  const statusNode = { textContent: "" };
  const controls = [{ disabled: false }];
  const form = {
    elements: controls,
    reportValidity: () => true,
    addEventListener: (type, callback) => {
      if (type === "submit") submitHandler = callback;
    },
  };
  const location = {
    pathname,
    search,
    hash,
    assign: url => assignedUrls.push(url),
  };
  const history = {
    replaceState: (_state, _title, url) => {
      replacedUrls.push(url);
      location.pathname = url;
      location.search = "";
      location.hash = "";
    },
  };
  const document = {
    body: { dataset: { page } },
    getElementById: id => {
      if (id === "form-status") return statusNode;
      if (id === "reset-form" || id === "verify-form") return form;
      return null;
    },
  };
  class FakeFormData {
    get(name) {
      return name === "password" ? password : null;
    }
  }
  const context = {
    URLSearchParams,
    Uint8Array,
    crypto: webcrypto,
    document,
    FormData: FakeFormData,
    history,
    window: {
      location,
      history,
      confirm: () => true,
    },
    fetch: async (url, options) => {
      requests.push({ url, options });
      return {
        ok: true,
        status: 200,
        json: async () => ({}),
      };
    },
    setTimeout: callback => callback(),
  };

  vm.runInNewContext(source, context, { filename: "app.js" });
  return {
    assignedUrls,
    controls,
    form,
    replacedUrls,
    requests,
    statusNode,
    submit: async () => {
      assert.equal(typeof submitHandler, "function");
      await submitHandler({ preventDefault: () => {} });
    },
  };
};

const reset = loadPage({
  page: "reset",
  pathname: "/redefinir-senha",
  search: "?utm_source=email",
  hash: "#token=reset%2Eseguro",
  password: "uma senha nova longa",
});
assert.deepEqual(reset.replacedUrls, ["/redefinir-senha"]);
assert.equal(reset.requests.length, 0);
await reset.submit();
assert.equal(reset.requests.length, 1);
assert.equal(reset.requests[0].url, "/api/v1/auth/password/reset");
assert.deepEqual(
  JSON.parse(reset.requests[0].options.body),
  { token: "reset.seguro", password: "uma senha nova longa" }
);

const verify = loadPage({
  page: "verify",
  pathname: "/verificar-email",
  hash: "#token=confirmacao%3Asegura",
});
assert.deepEqual(verify.replacedUrls, ["/verificar-email"]);
assert.equal(verify.requests.length, 0);
await verify.submit();
assert.equal(verify.requests.length, 1);
assert.equal(verify.requests[0].url, "/api/v1/auth/email/verify");
assert.deepEqual(
  JSON.parse(verify.requests[0].options.body),
  { token: "confirmacao:segura" }
);

const legacyQuery = loadPage({
  page: "verify",
  pathname: "/verificar-email",
  search: "?token=nao-pode-ser-usado",
});
assert.deepEqual(legacyQuery.replacedUrls, ["/verificar-email"]);
await legacyQuery.submit();
assert.equal(legacyQuery.requests.length, 0);
assert.match(legacyQuery.statusNode.textContent, /invalido|incompleto/);

console.log("PASS: tokens de acao ficam no fragmento, somem da URL e nao usam query");
