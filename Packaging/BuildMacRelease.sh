#!/bin/bash
# Empacota o ChurchDAW para macOS. Uma release exige Developer ID E notarizacao.
# Uso comercial: ./Packaging/BuildMacRelease.sh -b build-mac -i IDENTIDADE -p PERFIL -o SAIDA
# O perfil do notarytool deve existir no Keychain. Nenhuma senha vai na CLI.
# Uso local explicito, nunca para distribuicao: -b build-mac -u -o SAIDA
# A saida comercial so aparece depois de app, DMG e copia montada serem aprovados.
set -Eeuo pipefail

BUILD_DIR=""; IDENTITY=""; NOTARY_PROFILE=""; OUTPUT_DIR=""
APPLE_ID=""; TEAM_ID=""; UNSIGNED_TEST=0
# A chave permanece no disco; somente seu caminho e passado ao notarytool.
NOTARY_AUTH=()
if [[ -n "${NOTARY_KEY_PATH:-}" ]]; then
    [[ -f "$NOTARY_KEY_PATH" && -n "${NOTARY_KEY_ID:-}" && -n "${NOTARY_ISSUER_ID:-}" ]] || { echo "Credenciais de notarizacao incompletas" >&2; exit 1; }
    NOTARY_AUTH=(--key "$NOTARY_KEY_PATH" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER_ID")
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ENTITLEMENTS="$SCRIPT_DIR/ChurchDAW.entitlements"
fail() { printf 'erro: %s\n' "$*" >&2; exit 1; }
while getopts 'b:i:n:t:p:o:uh' opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;; i) IDENTITY="$OPTARG" ;;
        n) APPLE_ID="$OPTARG" ;; t) TEAM_ID="$OPTARG" ;;
        p) NOTARY_PROFILE="$OPTARG" ;; o) OUTPUT_DIR="$OPTARG" ;;
        u) UNSIGNED_TEST=1 ;;
        h) sed -n '2,6s/^# //p' "$0"; exit 0 ;;
        *) exit 2 ;;
    esac
done
[[ -n "$BUILD_DIR" ]] || fail 'informe o diretorio de build com -b'
if [[ -z "$NOTARY_PROFILE" && -n "$APPLE_ID" && -n "$TEAM_ID" ]]; then
    NOTARY_PROFILE=churchdaw
fi
if [[ "$UNSIGNED_TEST" == 1 ]]; then
    [[ -z "$IDENTITY" && -z "$NOTARY_PROFILE" ]] \
        || fail '-u nao pode ser combinado com assinatura ou notarizacao'
else
    [[ -n "$IDENTITY" && ( -n "$NOTARY_PROFILE" || ${#NOTARY_AUTH[@]} -gt 0 ) ]] \
        || fail 'release exige Developer ID (-i) e perfil de notarizacao (-p); -u existe apenas para teste local'
fi

if [[ ${#NOTARY_AUTH[@]} == 0 ]]; then NOTARY_AUTH=(--keychain-profile "$NOTARY_PROFILE"); fi

CACHE="$BUILD_DIR/CMakeCache.txt"
[[ -s "$CACHE" ]] || fail 'CMakeCache.txt ausente'
grep -Fxq 'CHURCHDAW_ENABLE_DESKTOP_LICENSING:BOOL=ON' "$CACHE" \
    || fail 'build sem licenciamento comercial habilitado'
if [[ "$UNSIGNED_TEST" == 0 ]]; then
    grep -Fxq 'CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE:BOOL=ON' "$CACHE" \
        || fail 'build comercial precisa exigir assinatura do sistema no auto-update'
fi

WORK="$(mktemp -d)"; MOUNT=""
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [[ -n "$MOUNT" ]]; then hdiutil detach "$MOUNT" >/dev/null 2>&1 || true; fi
    rm -rf "$WORK"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
find "$BUILD_DIR" -maxdepth 4 -name ChurchDAW.app -type d -print >"$WORK/apps.txt"
[[ "$(awk 'END { print NR }' "$WORK/apps.txt")" == 1 ]] \
    || fail 'o build precisa conter exatamente um ChurchDAW.app'
SOURCE_APP="$(cat "$WORK/apps.txt")"
PLIST="$SOURCE_APP/Contents/Info.plist"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")"
BUNDLE_ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST")"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && "$BUNDLE_ID" == com.churchdaw.app ]] \
    || fail 'versao ou bundle ID invalido'
SOURCE_EXE="$SOURCE_APP/Contents/MacOS/ChurchDAW"
[[ -x "$SOURCE_EXE" ]] || fail 'executavel ausente'
strings "$SOURCE_EXE" >"$WORK/strings.txt"
grep -Fq 'CHURCHDAW_RELEASE_LICENSE_GATE=DESKTOP_ENABLED' "$WORK/strings.txt" \
    || fail 'executavel sem marcador de licenciamento'
if [[ "$UNSIGNED_TEST" == 0 ]]; then
    if grep -Fq '[update] DMG sem assinatura do sistema; aceito' "$WORK/strings.txt"; then
        fail 'executavel aceita DMG sem assinatura do sistema'
    fi
    ARCHS="$(lipo -archs "$SOURCE_EXE")"
    [[ " $ARCHS " == *' arm64 '* && " $ARCHS " == *' x86_64 '* ]] \
        || fail 'release macOS precisa suportar Intel e Apple Silicon'
fi

OUTPUT_DIR="${OUTPUT_DIR:-$(dirname "$BUILD_DIR")/dist}"
mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd -P)"
NAME="ChurchDAW-$VERSION.dmg"
if [[ "$UNSIGNED_TEST" == 1 ]]; then NAME="ChurchDAW-$VERSION-NAO-PARA-DISTRIBUICAO.dmg"; fi
FINAL_DMG="$OUTPUT_DIR/$NAME"
[[ ! -e "$FINAL_DMG" ]] || fail "saida ja existe: $FINAL_DMG"
LOGS="$OUTPUT_DIR/ChurchDAW-$VERSION-notary-logs"
mkdir -p "$LOGS"
mkdir "$WORK/payload"
APP="$WORK/payload/ChurchDAW.app"
ditto "$SOURCE_APP" "$APP"
EXE="$APP/Contents/MacOS/ChurchDAW"

notarize() {
    local package="$1" label="$2" result="$LOGS/$2-submit.json"
    printf 'Notarizando %s...\n' "$label"
    xcrun notarytool submit "$package" "${NOTARY_AUTH[@]}" \
        --wait --output-format json >"$result"
    python3 - "$result" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]))
if p.get('status') != 'Accepted':
    raise SystemExit('Notarizacao nao aceita: '+str(p.get('status')))
if not p.get('id'): raise SystemExit('Notarizacao sem identificador')
print('Notarizacao aceita:',p['id'])
PY
    local submission
    submission="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$result")"
    xcrun notarytool log "$submission" "${NOTARY_AUTH[@]}" \
        "$LOGS/$label-log.json"
    python3 - "$LOGS/$label-log.json" <<'PY'
import json,sys
p=json.load(open(sys.argv[1]))
if p.get('status') != 'Accepted' or p.get('issues') or p.get('warnings') or p.get('problems'):
    raise SystemExit('O log do notario nao comprova uma release aprovada sem problemas')
PY
}

verify_app() {
    local app="$1" label="$2"
    codesign --verify --deep --strict --verbose=2 "$app" 2>&1 | tee "$LOGS/$label-signature.txt"
    xcrun stapler validate "$app" 2>&1 | tee "$LOGS/$label-ticket.txt"
    spctl --assess --type execute --verbose=4 "$app" 2>&1 | tee "$LOGS/$label-gatekeeper.txt"
}

if [[ "$UNSIGNED_TEST" == 0 ]]; then
    [[ -s "$ENTITLEMENTS" ]] || fail 'entitlements ausentes'
    plutil -lint "$ENTITLEMENTS" >/dev/null
    codesign --force --options runtime --timestamp --entitlements "$ENTITLEMENTS" \
        --sign "$IDENTITY" "$APP"
    codesign -d --verbose=4 "$APP" >"$LOGS/app-signing.txt" 2>&1
    grep -Fq 'Authority=Developer ID Application:' "$LOGS/app-signing.txt" \
        || fail 'assinatura nao e Developer ID Application'
    grep -Fq 'Timestamp=' "$LOGS/app-signing.txt" || fail 'assinatura sem timestamp'
    grep -Fq '(runtime)' "$LOGS/app-signing.txt" || fail 'Hardened Runtime ausente'
    codesign -d --entitlements :- "$APP" >"$WORK/entitlements.plist" 2>/dev/null
    python3 - "$WORK/entitlements.plist" "$ENTITLEMENTS" <<'PY'
import plistlib,sys
if plistlib.load(open(sys.argv[1],'rb')) != plistlib.load(open(sys.argv[2],'rb')):
    raise SystemExit('Entitlements da assinatura divergem dos aprovados')
PY
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$WORK/app.zip"
    notarize "$WORK/app.zip" app
    xcrun stapler staple "$APP"
    verify_app "$APP" app
fi

ln -s /Applications "$WORK/payload/Applications"
DMG="$WORK/$NAME"
hdiutil create -volname "ChurchDAW $VERSION" -srcfolder "$WORK/payload" -format UDZO "$DMG" >/dev/null
if [[ "$UNSIGNED_TEST" == 0 ]]; then
    codesign --force --timestamp --sign "$IDENTITY" "$DMG"
    notarize "$DMG" dmg
    xcrun stapler staple "$DMG"
    codesign --verify --strict --verbose=2 "$DMG"
    xcrun stapler validate "$DMG" 2>&1 | tee "$LOGS/dmg-ticket.txt"
    spctl --assess --type open --context context:primary-signature --verbose=4 "$DMG" \
        2>&1 | tee "$LOGS/dmg-gatekeeper.txt"
    # Clientes ate 1.6.30 ainda usam --type install. O mesmo DMG precisa passar
    # por eles para que consigam chegar ao atualizador corrigido em 1.6.31.
    spctl --assess --type install --verbose=4 "$DMG" \
        2>&1 | tee "$LOGS/dmg-gatekeeper-legacy.txt"
    mkdir "$WORK/mount"
    MOUNT="$WORK/mount"
    hdiutil attach -readonly -nobrowse -mountpoint "$MOUNT" "$DMG" >"$LOGS/mount.txt"
    verify_app "$MOUNT/ChurchDAW.app" mounted-app
    cmp "$EXE" "$MOUNT/ChurchDAW.app/Contents/MacOS/ChurchDAW" \
        || fail 'executavel montado diverge do app notarizado'
    hdiutil detach "$MOUNT" >"$LOGS/unmount.txt"
    MOUNT=""
fi

# O nome final e os campos para a policy nunca sao emitidos antes dos portoes.
ditto "$DMG" "$FINAL_DMG"
SHA="$(shasum -a 256 "$FINAL_DMG" | awk '{print $1}')"
BYTES="$(stat -f%z "$FINAL_DMG")"
printf '\nDMG: %s\nBytes: %s\nSHA-256: %s\n' "$FINAL_DMG" "$BYTES" "$SHA"
if [[ "$UNSIGNED_TEST" == 0 ]]; then
    python3 - "$FINAL_DMG.verification.json" "$VERSION" "$SHA" "$BYTES" <<'PYREPORT'
import json,sys
from pathlib import Path
path,version,sha,size=sys.argv[1:]
report={'schema_version':1,'product_id':'com.churchdaw.app','version':version,
        'sha256':sha,'bytes':int(size),'checks':{key:True for key in [
        'developer_id','secure_timestamp','hardened_runtime','app_notarization',
        'app_stapled','app_gatekeeper','dmg_notarization','dmg_stapled',
        'dmg_gatekeeper','legacy_dmg_gatekeeper','mounted_app_gatekeeper','mounted_executable_matches']}}
Path(path).write_text(json.dumps(report,indent=2)+'\n')
PYREPORT
    printf 'MAC_RELEASE_VERIFIED\n'
    printf 'mac_installer_url https://churchdaw.com/app/desktop/%s\n' "$NAME"
    printf 'mac_installer_sha256 %s\nmac_installer_bytes %s\n' "$SHA" "$BYTES"
else
    printf 'TESTE LOCAL: este DMG nao pode entrar na policy de atualizacao.\n'
fi
