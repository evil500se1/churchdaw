#!/bin/sh
set -eu

fail() {
    printf '%s\n' "SWITCH FAILED: $*" >&2
    exit 1
}

[ "$#" -eq 4 ] || fail \
    "uso: switch-release.sh PROJECT_ROOT PUBLIC_HTML APP_RELEASE PHP_BIN"

project_root_input=$1
public_html_input=$2
release_id=$3
php_bin=$4

case "$release_id" in
    ''|*[!A-Za-z0-9._-]*) fail "APP_RELEASE invalida" ;;
esac
[ "${#release_id}" -le 64 ] || fail "APP_RELEASE excede 64 caracteres"
[ -x "$php_bin" ] || fail "PHP_BIN nao e executavel"
[ -d "$project_root_input/releases/$release_id" ] ||
    fail "release privada inexistente"
[ -d "$public_html_input" ] || fail "DocumentRoot inexistente"

project_root=$(cd "$project_root_input" && pwd -P)
public_html=$(cd "$public_html_input" && pwd -P)
script_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
release_root="$project_root/releases/$release_id"
asset_root="$public_html/account-assets/$release_id"
current="$project_root/current"

[ "$project_root" != "/" ] || fail "PROJECT_ROOT amplo demais"
[ "$public_html" != "/" ] || fail "PUBLIC_HTML amplo demais"
[ -f "$public_html/churchdaw-account.php" ] ||
    fail "front-controller estavel ausente"
[ -f "$asset_root/app.css" ] && [ -f "$asset_root/app.js" ] ||
    fail "assets versionados ainda nao foram preparados"
cmp -s "$release_root/public/account-assets/app.css" "$asset_root/app.css" ||
    fail "app.css publico nao corresponde a release"
cmp -s "$release_root/public/account-assets/app.js" "$asset_root/app.js" ||
    fail "app.js publico nao corresponde a release"

"$php_bin" \
    "$script_root/preflight-release.php" \
    "$release_root" \
    "$release_id"

if [ -e "$current" ] || [ -L "$current" ]; then
    [ -L "$current" ] || fail "current existe, mas nao e link simbolico"
    previous=$(readlink "$current")
else
    previous='(nenhuma)'
fi

next="$project_root/.current-$release_id-$$"
case "$next" in
    "$project_root"/.current-*) ;;
    *) fail "link temporario inseguro" ;;
esac
[ ! -e "$next" ] && [ ! -L "$next" ] || fail "link temporario ja existe"

cleanup() {
    rm -f -- "$next" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

ln -s "releases/$release_id" "$next"
[ "$(readlink "$next")" = "releases/$release_id" ] ||
    fail "link temporario aponta para destino inesperado"

# A troca de rename no mesmo filesystem e atomica. O rollback usa este mesmo
# script com o APP_RELEASE anterior, cujos assets continuam retidos.
mv -Tf -- "$next" "$current"
trap - EXIT HUP INT TERM

resolved_current=$(cd "$current" && pwd -P)
resolved_release=$(cd "$release_root" && pwd -P)
[ "$resolved_current" = "$resolved_release" ] ||
    fail "verificacao pos-corte nao corresponde a release"

printf '%s\n' \
    "SWITCH OK release=$release_id previous=$previous" \
    "CHECKOUT_ENABLED=0 confirmado no preflight"
