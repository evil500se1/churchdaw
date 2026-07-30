#!/bin/sh
set -eu

fail() {
    printf '%s\n' "SMOKE FAILED: $*" >&2
    exit 1
}

[ "$#" -eq 5 ] || fail \
    "uso: smoke.sh BASE_URL APP_RELEASE CSS_SHA256 JS_SHA256 PHP_BIN"

base_url=${1%/}
release_id=$2
css_sha=$3
js_sha=$4
php_bin=$5

case "$base_url" in
    https://*) ;;
    *) fail "BASE_URL precisa usar HTTPS" ;;
esac
case "$release_id" in
    ''|*[!A-Za-z0-9._-]*) fail "APP_RELEASE invalida" ;;
esac
case "$css_sha:$js_sha" in
    *[!A-Fa-f0-9:]*|*::*|:*|*:) fail "hash SHA-256 invalido" ;;
esac
[ "${#css_sha}" -eq 64 ] && [ "${#js_sha}" -eq 64 ] ||
    fail "hash SHA-256 precisa ter 64 caracteres"
[ -x "$php_bin" ] || fail "PHP_BIN nao e executavel"

tmp_root=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$tmp_root/churchdaw-smoke.XXXXXX")
case "$tmp" in
    "$tmp_root"/churchdaw-smoke.*) ;;
    *) fail "diretorio temporario inseguro" ;;
esac

cleanup() {
    rm -f -- "$tmp"/* 2>/dev/null || true
    rmdir -- "$tmp" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

fetch() {
    name=$1
    path=$2
    shift 2
    curl -sS --max-redirs 0 \
        -o "$tmp/$name.body" \
        -D "$tmp/$name.headers" \
        -w '%{http_code}' \
        "$@" \
        "$base_url$path"
}

assert_status() {
    actual=$1
    expected=$2
    label=$3
    [ "$actual" = "$expected" ] ||
        fail "$label retornou HTTP $actual; esperado $expected"
}

assert_header() {
    name=$1
    pattern=$2
    grep -Eiq "$pattern" "$tmp/$name.headers" ||
        fail "$name sem header esperado: $pattern"
}

root_status=$(fetch root /)
assert_status "$root_status" 200 "landing page"
grep -qi 'ChurchDAW' "$tmp/root.body" ||
    fail "landing page existente nao foi reconhecida"

health_status=$(fetch health /api/v1/health)
assert_status "$health_status" 200 "health"
"$php_bin" -r '
    $data = json_decode(file_get_contents($argv[1]), true);
    $ok = is_array($data)
        && ($data["ok"] ?? false) === true
        && hash_equals($argv[2], (string) ($data["release"] ?? ""))
        && ($data["checkout_enabled"] ?? null) === false
        && is_array($data["schema"] ?? null)
        && ($data["schema"]["ready"] ?? false) === true
        && (string) ($data["schema"]["current"] ?? "") !== ""
        && (int) ($data["schema"]["applied_count"] ?? -1)
            === (int) ($data["schema"]["expected_count"] ?? -2)
        && ($data["schema"]["missing"] ?? null) === []
        && ($data["schema"]["unexpected"] ?? null) === []
        && hash_equals(
            (string) ($data["schema"]["current"] ?? ""),
            (string) ($data["schema"]["expected"] ?? "")
        );
    exit($ok ? 0 : 1);
' "$tmp/health.body" "$release_id" ||
    fail "health nao confirma release/schema ou CHECKOUT_ENABLED=0"
assert_header health '^cache-control:[[:space:]]*no-store'
assert_header health '^x-content-type-options:[[:space:]]*nosniff'
assert_header health '^x-frame-options:[[:space:]]*DENY'
assert_header health '^referrer-policy:[[:space:]]*no-referrer'
assert_header health '^permissions-policy:'
assert_header health '^content-security-policy:'
assert_header health '^strict-transport-security:'
assert_header health '^x-request-id:'

purchase_status=$(fetch purchase /comprar)
assert_status "$purchase_status" 503 "checkout pausado"
grep -qi 'temporariamente' "$tmp/purchase.body" ||
    fail "/comprar nao exibe manutencao com checkout pausado"
assert_header purchase '^cache-control:[[:space:]]*no-store'

for asset in app.css app.js; do
    name=$(printf '%s' "$asset" | tr '.' '-')
    status=$(fetch "$name" "/account-assets/$release_id/$asset")
    assert_status "$status" 200 "$asset"
    assert_header "$name" '^cache-control:.*max-age=31536000.*immutable'
    assert_header "$name" '^x-content-type-options:[[:space:]]*nosniff'
    assert_header "$name" '^cross-origin-resource-policy:[[:space:]]*same-origin'
done

actual_css=$(sha256sum "$tmp/app-css.body" | awk '{print $1}')
actual_js=$(sha256sum "$tmp/app-js.body" | awk '{print $1}')
[ "$actual_css" = "$(printf '%s' "$css_sha" | tr 'A-F' 'a-f')" ] ||
    fail "hash HTTP de app.css diverge"
[ "$actual_js" = "$(printf '%s' "$js_sha" | tr 'A-F' 'a-f')" ] ||
    fail "hash HTTP de app.js diverge"

direct_status=$(fetch direct-controller /churchdaw-account.php)
assert_status "$direct_status" 404 "front-controller direto"
direct_path_status=$(fetch direct-controller-path \
    /churchdaw-account.php/not-public)
assert_status "$direct_path_status" 404 "front-controller direto com PATH_INFO"
invalid_asset_status=$(fetch invalid-asset \
    "/account-assets/not-a-release/other.php")
assert_status "$invalid_asset_status" 404 "asset fora da allowlist"

for protected_path in /config/local.php /migrations/001_initial.sql; do
    name=$(printf '%s' "$protected_path" | tr '/.' '--')
    status=$(fetch "$name" "$protected_path")
    case "$status" in
        # HostGator/ModSecurity pode rejeitar nomes de migration com 406
        # antes de o Apache chegar ao nosso 403/404. Continua sendo fail
        # closed: nenhum byte privado e servido.
        403|404|406) ;;
        *) fail "$protected_path ficou acessivel (HTTP $status)" ;;
    esac
done

wrong_type_status=$(fetch wrong-type /api/v1/auth/login \
    -X POST -H 'Content-Type: text/plain' --data-binary '{}')
assert_status "$wrong_type_status" 415 "Content-Type invalido"

head -c 65537 /dev/zero | tr '\000' x > "$tmp/oversized.json"
oversized_status=$(fetch oversized /api/v1/auth/login \
    -X POST -H 'Content-Type: application/json' \
    --data-binary "@$tmp/oversized.json")
case "$oversized_status" in
    413)
        assert_header oversized \
            '^x-content-type-options:[[:space:]]*nosniff'
        ;;
    # O WAF da HostGator pode encerrar o corpo grande com 400 antes do
    # controller. Tambem e uma rejeicao segura; 2xx continua proibido.
    400) ;;
    *) fail "payload acima de 64 KiB retornou HTTP $oversized_status; esperado 400/413" ;;
esac

printf '%s\n' \
    "SMOKE OK release=$release_id checkout=disabled" \
    "app.css sha256=$actual_css" \
    "app.js sha256=$actual_js"
