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
    const paymentKey = "churchdaw_pix_payment";
    const payment = document.getElementById("pix-payment");
    const qr = document.getElementById("pix-qr");
    const code = document.getElementById("pix-code");
    const open = document.getElementById("pix-open");
    const live = document.getElementById("pix-live");
    let pollTimer = 0;
    let checkoutKey = sessionStorage.getItem(storageKey);
    if (!checkoutKey) {
      checkoutKey = idempotencyKey();
      sessionStorage.setItem(storageKey, checkoutKey);
    }
    const resetExpiredCheckout = message => {
      sessionStorage.removeItem(paymentKey);
      checkoutKey = idempotencyKey();
      sessionStorage.setItem(storageKey, checkoutKey);
      payment.hidden = true;
      form.hidden = false;
      busy(form, false);
      status(message || "Este Pix expirou. Gere um novo código para tentar novamente.");
    };
    const showPayment = payload => {
      form.hidden = true;
      payment.hidden = false;
      code.value = payload.pix_copy_paste || "";
      if (payload.qr_code_image) {
        qr.src = payload.qr_code_image;
        qr.hidden = false;
      } else {
        qr.hidden = true;
      }
      open.href = payload.checkout_url;
      open.hidden = !payload.checkout_url;
      status("Aguardando o pagamento Pix…", "pix-status");
      live.textContent = "Aguardando";
    };
    const poll = async payload => {
      window.clearTimeout(pollTimer);
      try {
        const current = await submitJson("/api/v1/checkout/status", {
          order_id: payload.order_id,
          checkout_token: payload.checkout_token,
        });
        status(current.message, "pix-status");
        if (current.paid && current.license_issued) {
          live.textContent = "Confirmado";
          live.classList.add("is-paid");
          sessionStorage.removeItem(paymentKey);
          sessionStorage.removeItem(storageKey);
          pollTimer = window.setTimeout(() => {
            window.location.assign("/compra/sucesso");
          }, 900);
          return;
        }
        if (current.status === "expired") {
          live.textContent = "Expirado";
          resetExpiredCheckout(current.message);
          return;
        }
        if (current.status === "paid_review") {
          live.textContent = "Em análise";
          pollTimer = window.setTimeout(() => poll(payload), 10000);
          return;
        }
      } catch (error) {
        status("A confirmação continua automática. Tentando novamente…", "pix-status");
      }
      pollTimer = window.setTimeout(() => poll(payload), 3000);
    };
    const savedPayment = sessionStorage.getItem(paymentKey);
    if (savedPayment) {
      try {
        const payload = JSON.parse(savedPayment);
        showPayment(payload);
        poll(payload);
      } catch {
        sessionStorage.removeItem(paymentKey);
      }
    }
    form?.querySelectorAll('input[name="plan_code"]').forEach(input => {
      input.addEventListener("change", () => {
        checkoutKey = idempotencyKey();
        sessionStorage.setItem(storageKey, checkoutKey);
      });
    });
    form?.addEventListener("submit", async event => {
      event.preventDefault();
      status("");
      if (!form.reportValidity()) return;
      // HTML omite controles disabled do FormData. Capture os valores antes
      // de travar o formulario para impedir envio duplo.
      const data = new FormData(form);
      busy(form, true);
      try {
        const payload = await submitJson("/api/v1/checkout/start", {
          display_name: data.get("display_name"),
          email: data.get("email"),
          password: data.get("password"),
          plan_code: data.get("plan_code"),
          accept_terms: data.get("accept_terms") === "on",
        }, { "Idempotency-Key": checkoutKey });
        sessionStorage.setItem(paymentKey, JSON.stringify(payload));
        showPayment(payload);
        poll(payload);
      } catch (error) {
        status(error.message);
        busy(form, false);
      }
    });
    document.getElementById("pix-copy")?.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code.value);
        status("Código Pix copiado.", "pix-status");
      } catch {
        code.focus();
        code.select();
        status("Código selecionado. Use Copiar no seu aparelho.", "pix-status");
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
      const data = new FormData(form);
      busy(form, true);
      try {
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
      const data = new FormData(form);
      busy(form, true);
      try {
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
      const data = new FormData(form);
      busy(form, true);
      try {
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
      const bundle = license?.activation_policy === "platform_bundle";
      document.getElementById("license-plan").textContent = bundle
        ? "Pacote completo · 1 Windows + 1 macOS + 1 Android"
        : "Licença antiga · um aparelho por vez";
      const devices = Array.isArray(license?.devices) ? license.devices : [];
      const devicesByPlatform = new Map(devices.map(device => [device.platform, device]));
      const slots = bundle
        ? [["windows", "Windows"], ["macos", "macOS"], ["android", "Android"]]
        : [[devices[0]?.platform || "single_device", "Aparelho ativo"]];
      const deviceList = document.getElementById("device-list");
      deviceList.replaceChildren();
      slots.forEach(([platform, label]) => {
        const device = bundle ? devicesByPlatform.get(platform) : devices[0];
        const card = document.createElement("article");
        card.className = `device-card account-device ${device ? "is-active" : "is-empty"}`;
        const icon = document.createElement("div");
        icon.className = "device-icon";
        icon.textContent = platform === "android" ? "▯" : "▣";
        const copy = document.createElement("div");
        const platformLabel = document.createElement("span");
        platformLabel.textContent = label.toUpperCase();
        const name = document.createElement("strong");
        name.textContent = device?.device_label || "Vaga livre";
        const detail = document.createElement("small");
        detail.textContent = device
          ? `Última validação: ${device.last_seen_at}`
          : "Disponível para ativação";
        copy.append(platformLabel, name, detail);
        card.append(icon, copy);
        if (device) {
          const deactivate = document.createElement("button");
          deactivate.className = "danger account-device-action";
          deactivate.type = "button";
          deactivate.dataset.deviceId = device.device_id;
          deactivate.textContent = "Liberar vaga";
          card.append(deactivate);
        }
        deviceList.append(card);
      });
      document.getElementById("transfer-note").textContent =
        `Trocas restantes neste período: ${payload.transfers_remaining_30d}.`;
      document.getElementById("account-loading").hidden = true;
      document.getElementById("account-content").hidden = false;
    };
    loadAccount().catch(error => status(error.message, "account-status"));

    document.getElementById("device-list")?.addEventListener("click", async event => {
      const button = event.target.closest("[data-device-id]");
      if (!button) return;
      if (!window.confirm("Liberar esta vaga? O ChurchDAW pedirá login novamente neste aparelho.")) return;
      button.disabled = true;
      try {
        await submitJson(
          "/api/v1/account/device/deactivate",
          { device_id: button.dataset.deviceId },
          { "X-CSRF-Token": csrfToken },
        );
        status("Vaga liberada. Você já pode ativar outro aparelho nessa plataforma.", "account-status");
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

  if (page === "admin") {
    const rows = Array.from(document.querySelectorAll("[data-admin-row]"));
    const search = document.getElementById("admin-search");
    const filter = document.getElementById("admin-status-filter");
    const count = document.getElementById("admin-result-count");
    const normalise = value => (value || "")
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .toLocaleLowerCase("pt-BR")
      .trim();
    const updateRows = () => {
      const term = normalise(search?.value);
      const selected = filter?.value || "all";
      let visible = 0;
      for (const row of rows) {
        const textMatches = term === "" || normalise(row.textContent).includes(term);
        const statusMatches = selected === "all"
          || row.dataset.licenseStatus === selected
          || row.dataset.userStatus === selected;
        row.hidden = !(textMatches && statusMatches);
        if (!row.hidden) visible += 1;
      }
      if (count) count.textContent = `${visible} resultado${visible === 1 ? "" : "s"}`;
    };
    search?.addEventListener("input", updateRows);
    filter?.addEventListener("change", updateRows);
    updateRows();

    for (const form of document.querySelectorAll("form[data-confirm]")) {
      form.addEventListener("submit", event => {
        if (!window.confirm(form.dataset.confirm || "Confirmar esta ação?")) {
          event.preventDefault();
        }
      });
    }

    document.addEventListener("click", event => {
      const open = event.target.closest(".admin-password-action");
      for (const details of document.querySelectorAll(".admin-password-action[open]")) {
        if (details !== open) details.removeAttribute("open");
      }
    });
  }
})();
