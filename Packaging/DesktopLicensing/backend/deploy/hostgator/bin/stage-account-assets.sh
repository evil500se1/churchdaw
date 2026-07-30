#!/bin/sh
set -eu

fail() {
    printf '%s\n' "STAGE FAILED: $*" >&2
    exit 1
}

[ "$#" -eq 3 ] || fail \
    "uso: stage-account-assets.sh PROJECT_ROOT PUBLIC_HTML APP_RELEASE"

project_root_input=$1
public_html_input=$2
release_id=$3

case "$release_id" in
    ''|*[!A-Za-z0-9._-]*) fail "APP_RELEASE invalida" ;;
esac
[ "${#release_id}" -le 64 ] || fail "APP_RELEASE excede 64 caracteres"

[ -d "$project_root_input/releases/$release_id" ] ||
    fail "release privada inexistente"
[ -d "$public_html_input" ] || fail "DocumentRoot inexistente"

project_root=$(cd "$project_root_input" && pwd -P)
public_html=$(cd "$public_html_input" && pwd -P)
release_root="$project_root/releases/$release_id"
source_root="$release_root/public/account-assets"
asset_root="$public_html/account-assets"
destination="$asset_root/$release_id"

[ "$project_root" != "/" ] || fail "PROJECT_ROOT amplo demais"
[ "$public_html" != "/" ] || fail "PUBLIC_HTML amplo demais"
[ -f "$source_root/app.css" ] && [ ! -L "$source_root/app.css" ] ||
    fail "app.css ausente ou simbolico"
[ -f "$source_root/app.js" ] && [ ! -L "$source_root/app.js" ] ||
    fail "app.js ausente ou simbolico"
[ -f "$asset_root/.htaccess" ] ||
    fail "instale primeiro o .htaccess estavel de account-assets"

if [ -e "$destination" ] || [ -L "$destination" ]; then
    [ -d "$destination" ] && [ ! -L "$destination" ] ||
        fail "destino existente nao e diretorio regular"
    cmp -s "$source_root/app.css" "$destination/app.css" &&
        cmp -s "$source_root/app.js" "$destination/app.js" ||
        fail "APP_RELEASE ja existe com bytes diferentes; use nova release"
    printf '%s\n' "ASSETS ALREADY READY release=$release_id"
    exit 0
fi

stage="$asset_root/.incoming-$release_id-$$"
case "$stage" in
    "$asset_root"/.incoming-*) ;;
    *) fail "diretorio temporario inseguro" ;;
esac
[ ! -e "$stage" ] && [ ! -L "$stage" ] || fail "staging ja existe"

cleanup() {
    rm -f -- "$stage/app.css" "$stage/app.js" 2>/dev/null || true
    rmdir -- "$stage" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM

mkdir -m 0755 -- "$stage"
cp -- "$source_root/app.css" "$stage/app.css"
cp -- "$source_root/app.js" "$stage/app.js"
chmod 0644 "$stage/app.css" "$stage/app.js"
cmp -s "$source_root/app.css" "$stage/app.css" ||
    fail "app.css mudou durante a copia"
cmp -s "$source_root/app.js" "$stage/app.js" ||
    fail "app.js mudou durante a copia"

# Mesmo filesystem + mv -T: o namespace versionado aparece inteiro de uma vez.
if ! mv -T -- "$stage" "$destination"; then
    if [ -d "$destination" ] && [ ! -L "$destination" ] &&
        cmp -s "$source_root/app.css" "$destination/app.css" &&
        cmp -s "$source_root/app.js" "$destination/app.js"; then
        printf '%s\n' "ASSETS READY BY CONCURRENT DEPLOY release=$release_id"
        exit 0
    fi
    fail "nao foi possivel publicar assets atomicamente"
fi
trap - EXIT HUP INT TERM

printf '%s\n' \
    "ASSETS READY release=$release_id" \
    "app.css sha256=$(sha256sum "$destination/app.css" | awk '{print $1}')" \
    "app.js sha256=$(sha256sum "$destination/app.js" | awk '{print $1}')"
