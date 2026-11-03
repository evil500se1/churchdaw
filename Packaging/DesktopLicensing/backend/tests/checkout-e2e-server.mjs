import fs from "node:fs";
import http from "node:http";
import { fileURLToPath } from "node:url";

const host = "127.0.0.1";
const port = Number.parseInt(process.env.CHURCHDAW_CHECKOUT_TEST_PORT || "4173", 10);
const appSource = fs.readFileSync(
  fileURLToPath(new URL("../public/account-assets/app.js", import.meta.url)),
  "utf8"
);
let lastCheckout = null;

const html = `<!doctype html>
<html lang="pt-BR">
<head><meta charset="utf-8"><title>ChurchDAW Checkout E2E</title></head>
<body data-page="purchase">
  <main>
    <form id="purchase-form" novalidate>
      <label>Plano <input type="radio" name="plan_code" value="complete" checked required></label>
      <label>Nome completo <input name="display_name" required></label>
      <label>E-mail <input name="email" type="email" required></label>
      <label>Crie uma senha <input name="password" type="password" minlength="8" required></label>
      <label><input name="accept_terms" type="checkbox" required> Li e aceito os termos</label>
      <button type="submit">Gerar Pix seguro</button>
      <p id="form-status" role="status"></p>
    </form>
    <section id="pix-payment" hidden>
      <span id="pix-live">Aguardando</span>
      <img id="pix-qr" alt="QR Code Pix" hidden>
      <textarea id="pix-code" readonly></textarea>
      <button id="pix-copy" type="button">Copiar código Pix</button>
      <a id="pix-open">Abrir pagamento</a>
      <p id="pix-status" role="status"></p>
    </section>
  </main>
  <script src="/account-assets/test/app.js"></script>
</body>
</html>`;

const readBody = request => new Promise((resolve, reject) => {
  const chunks = [];
  request.on("data", chunk => chunks.push(chunk));
  request.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
  request.on("error", reject);
});

const json = (response, status, payload) => {
  response.writeHead(status, {
    "Cache-Control": "no-store",
    "Content-Type": "application/json; charset=utf-8",
  });
  response.end(JSON.stringify(payload));
};

const server = http.createServer(async (request, response) => {
  if (request.method === "GET" && request.url === "/comprar") {
    response.writeHead(200, { "Content-Type": "text/html; charset=utf-8" });
    response.end(html);
    return;
  }
  if (request.method === "GET" && request.url === "/account-assets/test/app.js") {
    response.writeHead(200, { "Content-Type": "text/javascript; charset=utf-8" });
    response.end(appSource);
    return;
  }
  if (request.method === "GET" && request.url === "/__last-checkout") {
    json(response, 200, lastCheckout ?? {});
    return;
  }
  if (request.method === "POST" && request.url === "/api/v1/checkout/start") {
    lastCheckout = JSON.parse(await readBody(request));
    json(response, 200, {
      order_id: "order-e2e",
      checkout_token: "checkout-e2e",
      checkout_url: "https://example.invalid/pix",
      pix_copy_paste: "PIX-E2E-OK",
      qr_code_image: "",
    });
    return;
  }
  if (request.method === "POST" && request.url === "/api/v1/checkout/status") {
    json(response, 200, {
      paid: false,
      license_issued: false,
      status: "pending",
      message: "Aguardando pagamento de teste",
    });
    return;
  }
  response.writeHead(404);
  response.end("Not found");
});

server.listen(port, host, () => {
  console.log(`READY http://${host}:${port}/comprar`);
});
