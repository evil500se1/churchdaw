(() => {
  "use strict";

  // Shared behavior for purchase, account, and recovery routes.
  const page = document.body.dataset.page;
  const status = (message, target = "form-status") => {
    const node = document.getElementById(target);
    if (node) node.textContent = message || "";
  };
  const submitJson = async (url, body, extraHeaders = {}) => {
    const response = await fetch(url, {
      method: "POST",
      credentials: "same-origin",
      headers: { "Content-Type": "application/json", "Accept": "application/json", ...extraHeaders },
      body: JSON.stringify(body),
    });
    const payload = response.status === 204 ? {} : await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(payload.message || "Não foi possível concluir.");
    return payload;
  };
  const busy = (form, value) => {
    for (const control of form.elements) control.disabled = value;
  };
  const idempotencyKey = () => {
    const bytes = crypto.getRandomValues(new Uint8Array(24));
    return Array.from(bytes, value => value.toString(16).padStart(2, "0")).join("");
  };
  const consumeActionToken = () => {
    const fragment = window.location.hash.startsWith("#")
      ? window.location.hash.slice(1)
      : "";
    const token = new URLSearchParams(fragment).get("token") || "";
    history.replaceState(null, "", window.location.pathname);
    return token;
  };
  const actionToken = page === "reset" || page === "verify"
    ? consumeActionToken()
    : "";

  if (page === "purchase") {
    const form = document.getElementById("purchase-form");
    const storageKey = "churchdaw_checkout_idempotency";
    let checkoutKey = sessionStorage.getItem(storageKey);
    if (!checkoutKey) {
      checkoutKey = idempotencyKey();
      sessionStorage.setItem(storageKey, checkoutKey);
    }
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      status("");
      if (!form.reportValidity()) return;
      busy(form, true);
      try {
        const data = new FormData(form);
        const payload = await submitJson("/api/v1/checkout/start", {
          display_name: data.get("display_name"),
          email: data.get("email"),
          password: data.get("password"),
          accept_terms: data.get("accept_terms") === "on",
        }, { "Idempotency-Key": checkoutKey });
        status("Abrindo o pagamento seguro…");
        window.location.assign(payload.checkout_url);
      } catch (error) {
        status(error.message);
        busy(form, false);
      }
    });
  }

  if (["/compra/sucesso", "/compra/pendente", "/compra/falhou"].includes(window.location.pathname)) {
    sessionStorage.removeItem("churchdaw_checkout_idempotency");
  }

  if (page === "login") {
    const form = document.getElementById("login-form");
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      status("");
      if (!form.reportValidity()) return;
      busy(form, true);
      try {
        const data = new FormData(form);
        await submitJson("/api/v1/web/login", {
          email: data.get("email"),
          password: data.get("password"),
        });
        window.location.assign("/minha-conta");
      } catch (error) {
        status(error.message);
        busy(form, false);
      }
    });
  }

  if (page === "reset") {
    const form = document.getElementById("reset-form");
    if (!actionToken) {
      status("Este link de redefinicao e invalido ou esta incompleto.");
    }
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      status("");
      if (!actionToken) {
        status("Este link de redefinicao e invalido ou esta incompleto.");
        return;
      }
      if (!form.reportValidity()) return;
      busy(form, true);
      try {
        const data = new FormData(form);
        await submitJson("/api/v1/auth/password/reset", {
          token: actionToken,
          password: data.get("password"),
        });
        status("Senha atualizada. Você já pode entrar.");
        setTimeout(() => window.location.assign("/entrar"), 900);
      } catch (error) {
        status(error.message);
        busy(form, false);
      }
    });
  }

  if (page === "forgot" || page === "resend") {
    const form = document.getElementById(page === "forgot" ? "forgot-form" : "resend-form");
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      status("");
      if (!form.reportValidity()) return;
      busy(form, true);
      try {
        const data = new FormData(form);
        const endpoint = page === "forgot"
          ? "/api/v1/auth/password/forgot"
          : "/api/v1/auth/email/resend";
        await submitJson(endpoint, { email: data.get("email") });
        status(page === "forgot"
          ? "Se a conta existir, enviaremos as instruções."
          : "Se a conta ainda precisar, enviaremos um novo link.");
      } catch (error) {
        status(error.message);
      } finally {
        busy(form, false);
      }
    });
  }

  if (page === "verify") {
    const form = document.getElementById("verify-form");
    if (!actionToken) {
      status("Este link de confirmacao e invalido ou esta incompleto.");
    }
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      busy(form, true);
      status("");
      if (!actionToken) {
        status("Este link de confirmacao e invalido ou esta incompleto.");
        busy(form, false);
        return;
      }
      try {
        await submitJson("/api/v1/auth/email/verify", {
          token: actionToken,
        });
        status("E-mail confirmado. Você já pode entrar.");
        setTimeout(() => window.location.assign("/entrar"), 900);
      } catch (error) {
        status(error.message);
        busy(form, false);
      }
    });
  }

  if (page === "account") {
    let csrfToken = "";
    const loadAccount = async () => {
      const response = await fetch("/api/v1/account", {
        credentials: "same-origin",
        headers: { "Accept": "application/json" },
      });
      if (response.status === 401) {
        window.location.assign("/entrar");
        return;
      }
      const payload = await response.json();
      if (!response.ok) throw new Error(payload.message || "Não foi possível abrir a conta.");
      csrfToken = payload.csrf_token;
      document.getElementById("account-name").textContent = payload.user.display_name;
      document.getElementById("account-email").textContent = payload.user.email;
      const license = payload.license;
      document.getElementById("license-status").textContent =
        license?.status === "active" ? "LICENÇA ATIVA" : "SEM LICENÇA";
      document.getElementById("device-label").textContent =
        license?.device_label || "Nenhum computador ativo";
      document.getElementById("device-detail").textContent =
        license?.platform ? `${license.platform} · última validação ${license.last_seen_at}` : "";
      document.getElementById("transfer-note").textContent =
        `Trocas restantes neste período: ${payload.transfers_remaining_30d}.`;
      document.getElementById("deactivate-button").disabled = !license?.device_label;
      document.getElementById("account-loading").hidden = true;
      document.getElementById("account-content").hidden = false;
    };
    loadAccount().catch(error => status(error.message, "account-status"));

    document.getElementById("deactivate-button")?.addEventListener("click", async event => {
      const button = event.currentTarget;
      if (!window.confirm("Desativar este computador? O ChurchDAW pedirá login na próxima abertura.")) return;
      button.disabled = true;
      try {
        await submitJson("/api/v1/account/device/deactivate", {}, { "X-CSRF-Token": csrfToken });
        status("Computador desativado. Você já pode ativar outro.", "account-status");
        await loadAccount();
      } catch (error) {
        status(error.message, "account-status");
        button.disabled = false;
      }
    });
    document.getElementById("logout-button")?.addEventListener("click", async () => {
      try { await submitJson("/api/v1/web/logout", {}, { "X-CSRF-Token": csrfToken }); } catch (_) {}
      window.location.assign("/entrar");
    });
  }
})();
