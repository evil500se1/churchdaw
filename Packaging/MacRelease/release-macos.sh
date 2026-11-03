#!/usr/bin/env bash

# Produz o unico candidato macOS apto a distribuicao comercial.
# Este script deve rodar em um checkout limpo de uma tag e falha se qualquer
# assinatura, entitlement, versao, notarizacao ou avaliacao do Gatekeeper
# divergir do esperado.

set -Eeuo pipefail
IFS=$'\n\t'
umask 077

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "comando obrigatorio ausente: $1"
}

require_env() {
    local name="$1"
    [[ -n "${!name:-}" ]] || fail "variavel obrigatoria vazia: $name"
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$repo_root"

require_env RELEASE_TAG
require_env BUILD_NUMBER
require_env EXPECTED_BUNDLE_ID
require_env MACOSX_DEPLOYMENT_TARGET
require_env RELEASE_OUTPUT_ROOT
require_env MACOS_CERTIFICATE_P12_BASE64
require_env MACOS_CERTIFICATE_P12_PASSWORD
require_env APPLE_TEAM_ID
require_env MACOS_EXPECTED_TEAM_ID
require_env MACOS_EXPECTED_CERTIFICATE_SHA256
require_env APPLE_NOTARY_KEY_P8_BASE64
require_env APPLE_NOTARY_KEY_ID
require_env APPLE_NOTARY_ISSUER_ID

[[ "$RELEASE_TAG" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] \
    || fail "RELEASE_TAG deve usar exatamente vMAJOR.MINOR.PATCH"
release_version="${RELEASE_TAG#v}"
[[ "$BUILD_NUMBER" =~ ^[1-9][0-9]*$ ]] \
    || fail "BUILD_NUMBER deve ser um inteiro positivo"
[[ "$EXPECTED_BUNDLE_ID" =~ ^[A-Za-z0-9][A-Za-z0-9.-]+$ ]] \
    || fail "EXPECTED_BUNDLE_ID invalido"
[[ "$MACOSX_DEPLOYMENT_TARGET" =~ ^[0-9]+\.[0-9]+$ ]] \
    || fail "MACOSX_DEPLOYMENT_TARGET invalido"
[[ "$MACOSX_DEPLOYMENT_TARGET" == "10.15" ]] \
    || fail "o release desktop deve manter Catalina 10.15 no slice Intel"
[[ "$APPLE_TEAM_ID" =~ ^[A-Z0-9]{10}$ ]] \
    || fail "APPLE_TEAM_ID deve conter 10 caracteres A-Z/0-9"
[[ "$MACOS_EXPECTED_TEAM_ID" =~ ^[A-Z0-9]{10}$ ]] \
    || fail "MACOS_EXPECTED_TEAM_ID deve conter 10 caracteres A-Z/0-9"
[[ "$APPLE_TEAM_ID" == "$MACOS_EXPECTED_TEAM_ID" ]] \
    || fail "APPLE_TEAM_ID diverge do Team ID comercial fixado"
[[ "$MACOS_EXPECTED_CERTIFICATE_SHA256" =~ ^[0-9A-Fa-f]{64}$ ]] \
    || fail "MACOS_EXPECTED_CERTIFICATE_SHA256 deve conter 64 hexadecimais"
[[ "$APPLE_NOTARY_KEY_ID" =~ ^[A-Z0-9]{10}$ ]] \
    || fail "APPLE_NOTARY_KEY_ID deve conter 10 caracteres A-Z/0-9"
[[ "$APPLE_NOTARY_ISSUER_ID" =~ ^[0-9A-Fa-f-]{36}$ ]] \
    || fail "APPLE_NOTARY_ISSUER_ID nao parece um UUID"

case "$RELEASE_OUTPUT_ROOT" in
    /*) output_root="$RELEASE_OUTPUT_ROOT" ;;
    *) output_root="$repo_root/$RELEASE_OUTPUT_ROOT" ;;
esac

[[ ! -e "$output_root" ]] \
    || fail "diretorio de saida ja existe; use um runner limpo: $output_root"

for tool in bash cmake codesign cp ctest ditto file git hdiutil lipo openssl \
            otool plutil python3 security shasum spctl stat sw_vers vtool \
            xcodebuild xcrun strings; do
    require_command "$tool"
done
[[ -x /usr/libexec/PlistBuddy ]] || fail "PlistBuddy ausente"

runner_temp="${RUNNER_TEMP:-/tmp}"
temp_root="$(mktemp -d "$runner_temp/churchdaw-macos-release.XXXXXX")"
keychain_path="$temp_root/signing.keychain-db"
p12_path="$temp_root/developer-id.p12"
notary_key_path="$temp_root/AuthKey_${APPLE_NOTARY_KEY_ID}.p8"
certificate_pem="$temp_root/developer-id.pem"
mount_dir=""

cleanup() {
    local rc=$?
    set +e
    if [[ -n "$mount_dir" ]] && mount | grep -Fq " on $mount_dir "; then
        hdiutil detach "$mount_dir" >/dev/null 2>&1
    fi
    security delete-keychain "$keychain_path" >/dev/null 2>&1
    rm -rf "$temp_root"
    return "$rc"
}
trap cleanup EXIT

tag_commit="$(git rev-parse --verify "refs/tags/$RELEASE_TAG^{commit}")" \
    || fail "tag nao existe no checkout: $RELEASE_TAG"
source_commit="$(git rev-parse HEAD)"
[[ "$tag_commit" == "$source_commit" ]] \
    || fail "HEAD ($source_commit) nao corresponde a tag $RELEASE_TAG ($tag_commit)"
[[ -z "$(git status --porcelain=v1 --untracked-files=all)" ]] \
    || fail "checkout possui alteracoes ou arquivos nao rastreados"

cmake_version="$(
    sed -nE 's/^project\(MinhaDAW VERSION ([0-9]+\.[0-9]+\.[0-9]+)\)$/\1/p' \
        CMakeLists.txt
)"
[[ "$cmake_version" == "$release_version" ]] \
    || fail "tag $release_version diverge do project() $cmake_version"

logs_dir="$output_root/logs"
artifacts_dir="$output_root/artifacts"
mkdir -p "$logs_dir" "$artifacts_dir"

exec > >(tee "$logs_dir/release.log") 2>&1

{
    printf 'release_tag=%s\n' "$RELEASE_TAG"
    printf 'release_version=%s\n' "$release_version"
    printf 'build_number=%s\n' "$BUILD_NUMBER"
    printf 'source_commit=%s\n' "$source_commit"
    printf 'bundle_id=%s\n' "$EXPECTED_BUNDLE_ID"
    printf 'deployment_target=%s\n' "$MACOSX_DEPLOYMENT_TARGET"
    printf 'runner=%s\n' "${RUNNER_NAME:-unknown}"
    sw_vers
    xcodebuild -version
    cmake --version | head -n 1
} | tee "$logs_dir/environment.txt"

python3 - "$p12_path" "$notary_key_path" <<'PY'
import base64
import binascii
import os
import pathlib
import sys

for env_name, destination in (
    ("MACOS_CERTIFICATE_P12_BASE64", sys.argv[1]),
    ("APPLE_NOTARY_KEY_P8_BASE64", sys.argv[2]),
):
    try:
        raw = base64.b64decode(os.environ[env_name], validate=True)
    except (ValueError, binascii.Error) as exc:
        raise SystemExit(f"{env_name} nao contem base64 valido: {exc}")
    if not raw:
        raise SystemExit(f"{env_name} decodificou para um arquivo vazio")
    pathlib.Path(destination).write_bytes(raw)
PY
chmod 600 "$p12_path" "$notary_key_path"

keychain_password="$(openssl rand -base64 48)"
printf '::add-mask::%s\n' "$keychain_password"
security create-keychain -p "$keychain_password" "$keychain_path"
security set-keychain-settings -lut 7200 "$keychain_path"
security unlock-keychain -p "$keychain_password" "$keychain_path"
security import "$p12_path" \
    -k "$keychain_path" \
    -P "$MACOS_CERTIFICATE_P12_PASSWORD" \
    -T /usr/bin/codesign \
    -T /usr/bin/security
security set-key-partition-list \
    -S apple-tool:,apple:,codesign: \
    -s \
    -k "$keychain_password" \
    "$keychain_path" >/dev/null

security find-identity -v -p codesigning "$keychain_path" \
    | tee "$logs_dir/codesigning-identities.txt"

identity_record="$(
    security find-identity -v -p codesigning "$keychain_path" \
        | sed -nE \
            "s/^[[:space:]]*[0-9]+\\) ([0-9A-Fa-f]{40}) \"(Developer ID Application: .+ \\(${APPLE_TEAM_ID}\\))\"$/\\1|\\2/p"
)"
identity_count="$(printf '%s\n' "$identity_record" | awk 'NF { count++ } END { print count + 0 }')"
[[ "$identity_count" -eq 1 ]] \
    || fail "esperava uma unica identidade Developer ID Application do Team ID $APPLE_TEAM_ID; encontrei $identity_count"
IFS='|' read -r signing_identity_sha1 signing_identity <<<"$identity_record"
signing_identity_sha1="$(
    printf '%s' "$signing_identity_sha1" | tr '[:lower:]' '[:upper:]'
)"
[[ "$signing_identity_sha1" =~ ^[0-9A-F]{40}$ ]] \
    || fail "SHA-1 da identidade Developer ID e invalido"

all_certificates_pem="$temp_root/all-imported-certificates.pem"
certificate_parts_dir="$temp_root/imported-certificates"
mkdir -p "$certificate_parts_dir"
security find-certificate -a -p "$keychain_path" >"$all_certificates_pem"
python3 - "$all_certificates_pem" "$certificate_parts_dir" <<'PY'
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="ascii")
destination = pathlib.Path(sys.argv[2])
begin = "-----BEGIN CERTIFICATE-----"
end = "-----END CERTIFICATE-----"
certificates = []
for suffix in source.split(begin)[1:]:
    body, separator, _tail = suffix.partition(end)
    if not separator:
        raise SystemExit("PEM importado contem certificado truncado")
    certificates.append(begin + body + end + "\n")
if not certificates:
    raise SystemExit("keychain temporario nao contem certificados")
for index, certificate in enumerate(certificates):
    (destination / f"certificate-{index:03d}.pem").write_text(
        certificate,
        encoding="ascii",
    )
PY

matching_certificate_count=0
for candidate_certificate in "$certificate_parts_dir"/*.pem; do
    [[ -f "$candidate_certificate" ]] || continue
    candidate_sha1="$(
        openssl x509 -in "$candidate_certificate" -noout -fingerprint -sha1 \
            | awk -F= '{ print $2 }' \
            | tr -d ':[:space:]' \
            | tr '[:lower:]' '[:upper:]'
    )"
    if [[ "$candidate_sha1" == "$signing_identity_sha1" ]]; then
        cp "$candidate_certificate" "$certificate_pem"
        matching_certificate_count=$((matching_certificate_count + 1))
    fi
done
[[ "$matching_certificate_count" -eq 1 && -s "$certificate_pem" ]] \
    || fail "nao foi possivel vincular univocamente a identidade ao certificado importado"
openssl x509 -in "$certificate_pem" -noout \
    -subject -issuer -serial -dates -fingerprint -sha256 \
    | tee "$logs_dir/developer-id-certificate.txt"
openssl x509 -in "$certificate_pem" -checkend 604800 -noout \
    || fail "certificado Developer ID expira em menos de 7 dias"
certificate_subject="$(openssl x509 -in "$certificate_pem" -noout -subject -nameopt RFC2253)"
grep -Fq "CN=$signing_identity" <<<"$certificate_subject" \
    || fail "CN do certificado diverge da identidade selecionada"
grep -Fq "OU=$APPLE_TEAM_ID" <<<"$certificate_subject" \
    || fail "OU do certificado diverge do APPLE_TEAM_ID"
certificate_sha256="$(
    openssl x509 -in "$certificate_pem" -outform DER \
        | shasum -a 256 \
        | awk '{print $1}'
)"
certificate_sha256_upper="$(
    printf '%s' "$certificate_sha256" | tr '[:lower:]' '[:upper:]'
)"
expected_certificate_sha256_upper="$(
    printf '%s' "$MACOS_EXPECTED_CERTIFICATE_SHA256" \
        | tr '[:lower:]' '[:upper:]'
)"
[[ "$certificate_sha256_upper" == "$expected_certificate_sha256_upper" ]] \
    || fail "fingerprint SHA-256 do Developer ID diverge do valor fixado"
security verify-cert -c "$certificate_pem" -p codeSign \
    | tee "$logs_dir/certificate-trust.txt"

require_signed_leaf_certificate() {
    local target="$1"
    local extracted_dir
    local extracted_sha256
    extracted_dir="$(mktemp -d "$temp_root/signed-leaf.XXXXXX")"
    (
        cd "$extracted_dir"
        codesign --display --extract-certificates "$target" >/dev/null 2>&1
    )
    [[ -s "$extracted_dir/codesign0" ]] \
        || fail "codesign nao extraiu o certificado leaf de $target"
    extracted_sha256="$(
        shasum -a 256 "$extracted_dir/codesign0" | awk '{ print $1 }' \
            | tr '[:lower:]' '[:upper:]'
    )"
    [[ "$extracted_sha256" == "$expected_certificate_sha256_upper" ]] \
        || fail "certificado leaf assinado diverge do pin SHA-256: $target"
}

xcrun notarytool history \
    --key "$notary_key_path" \
    --key-id "$APPLE_NOTARY_KEY_ID" \
    --issuer "$APPLE_NOTARY_ISSUER_ID" \
    --output-format json >"$logs_dir/notary-history-preflight.json"
python3 -m json.tool "$logs_dir/notary-history-preflight.json" >/dev/null \
    || fail "notarytool history nao retornou JSON valido"

build_dir="$repo_root/build-mac-commercial"
[[ ! -e "$build_dir" ]] || fail "diretorio de build comercial ja existe"

cmake -S "$repo_root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCHURCHDAW_ENABLE_DESKTOP_LICENSING=ON \
    -DCHURCHDAW_BUILD_LICENSE_TESTS=ON \
    -DCHURCHDAW_BUILD_AUDIO_TESTS=ON \
    -DCHURCHDAW_BUILD_UPDATE_TESTS=ON \
    -DCHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST=OFF \
    -DCHURCHDAW_UPDATE_ALLOW_LOCALHOST=OFF \
    -DCHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE=ON \
    2>&1 | tee "$logs_dir/cmake-configure.log"
grep -Fxq 'CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE:BOOL=ON' \
    "$build_dir/CMakeCache.txt" \
    || fail "build comercial nao exige assinatura do sistema no auto-update"
cmake --build "$build_dir" --config Release --parallel 3 \
    2>&1 | tee "$logs_dir/cmake-build.log"

ctest --test-dir "$build_dir" -C Release --output-on-failure \
    2>&1 | tee "$logs_dir/ctest.log"

license_test="$(
    find "$build_dir" -type f -name ChurchDAWLicenseVerifierTests -perm -111 -print -quit
)"
[[ -n "$license_test" ]] || fail "ChurchDAWLicenseVerifierTests nao foi gerado"
"$license_test" \
    "$repo_root/Tests/DesktopLicensing/lease-fixture-ci.token" \
    fixture-installation-id \
    1800000000 \
    2>&1 | tee "$logs_dir/license-verifier.log"

app="$build_dir/MinhaDAW_artefacts/Release/ChurchDAW.app"
executable="$app/Contents/MacOS/ChurchDAW"
plist="$app/Contents/Info.plist"
entitlements="$repo_root/Packaging/ChurchDAW.entitlements"
[[ -d "$app" ]] || fail "bundle nao encontrado: $app"
[[ -x "$executable" ]] || fail "executavel nao encontrado: $executable"
if strings "$executable" \
    | grep -F '[update] DMG sem assinatura do sistema; aceito' >/dev/null; then
    fail "binario comercial contem o ramo que aceita DMG sem assinatura"
fi
[[ -s "$plist" ]] || fail "Info.plist ausente"
[[ -s "$entitlements" ]] || fail "entitlements ausente"
plutil -lint "$plist"
plutil -lint "$entitlements"

plist_read() {
    /usr/libexec/PlistBuddy -c "Print :$1" "$plist"
}

[[ "$(plist_read CFBundleIdentifier)" == "$EXPECTED_BUNDLE_ID" ]] \
    || fail "CFBundleIdentifier inesperado"
[[ "$(plist_read CFBundleShortVersionString)" == "$release_version" ]] \
    || fail "CFBundleShortVersionString diverge da tag"
[[ "$(plist_read CFBundleExecutable)" == "ChurchDAW" ]] \
    || fail "CFBundleExecutable inesperado"
[[ "$(plist_read CFBundlePackageType)" == "APPL" ]] \
    || fail "CFBundlePackageType inesperado"
[[ -n "$(plist_read NSMicrophoneUsageDescription)" ]] \
    || fail "NSMicrophoneUsageDescription vazio"
[[ -n "$(plist_read NSBluetoothAlwaysUsageDescription)" ]] \
    || fail "NSBluetoothAlwaysUsageDescription vazio"

/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $BUILD_NUMBER" "$plist" \
    || /usr/libexec/PlistBuddy -c "Add :CFBundleVersion string $BUILD_NUMBER" "$plist"
[[ "$(plist_read CFBundleVersion)" == "$BUILD_NUMBER" ]] \
    || fail "CFBundleVersion nao recebeu o build monotonicamente crescente"
[[ "$(plist_read LSMinimumSystemVersion)" == "$MACOSX_DEPLOYMENT_TARGET" ]] \
    || fail "LSMinimumSystemVersion diverge do deployment target"
[[ "$(plist_read LSMinimumSystemVersionByArchitecture:x86_64)" == "$MACOSX_DEPLOYMENT_TARGET" ]] \
    || fail "piso x86_64 do Info.plist diverge do deployment target"
[[ "$(plist_read LSMinimumSystemVersionByArchitecture:arm64)" == "11.0" ]] \
    || fail "piso arm64 do Info.plist precisa ser 11.0"

architectures="$(lipo -archs "$executable")"
grep -qw arm64 <<<"$architectures" || fail "slice arm64 ausente"
grep -qw x86_64 <<<"$architectures" || fail "slice x86_64 ausente"
[[ "$(wc -w <<<"$architectures" | tr -d ' ')" -eq 2 ]] \
    || fail "arquiteturas inesperadas: $architectures"

vtool -show-build "$executable" | tee "$logs_dir/macho-build-versions.txt"
minos_dir="$temp_root/minos-slices"
mkdir -p "$minos_dir"
lipo "$executable" -thin x86_64 -output "$minos_dir/ChurchDAW-x86_64"
lipo "$executable" -thin arm64 -output "$minos_dir/ChurchDAW-arm64"

require_slice_minos() {
    local binary="$1"
    local expected="$2"
    local architecture="$3"
    local actual platform
    actual="$(
        vtool -show-build "$binary" \
            | awk '$1 == "minos" { print $2; exit }'
    )"
    platform="$(
        vtool -show-build "$binary" \
            | awk '$1 == "platform" { print $2; exit }'
    )"
    [[ "$actual" == "$expected" ]] \
        || fail "$architecture minos=$actual; esperado=$expected"
    [[ "$platform" == "MACOS" ]] \
        || fail "$architecture usa plataforma Mach-O inesperada: $platform"
    printf '%s minos=%s platform=%s\n' "$architecture" "$actual" "$platform"
}

# Catalina e o piso do produto Intel; Apple Silicon nasceu no macOS 11.
require_slice_minos \
    "$minos_dir/ChurchDAW-x86_64" "$MACOSX_DEPLOYMENT_TARGET" x86_64
require_slice_minos "$minos_dir/ChurchDAW-arm64" "11.0" arm64

otool -L "$executable" | tee "$logs_dir/macho-dependencies.txt"
if grep -Eq '/Users/|build-mac-commercial' "$logs_dir/macho-dependencies.txt"; then
    fail "dependencia absoluta do ambiente de build encontrada"
fi

sign_one() {
    local target="$1"
    codesign \
        --force \
        --sign "$signing_identity_sha1" \
        --keychain "$keychain_path" \
        --options runtime \
        --timestamp \
        "$target"
    require_signed_leaf_certificate "$target"
}

# Assina primeiro cada Mach-O, depois os bundles aninhados do mais profundo
# para o mais raso, e por ultimo o app com os entitlements explicitamente
# permitidos. --deep nunca e usado para assinar.
while IFS= read -r candidate; do
    if file -b "$candidate" | grep -q 'Mach-O'; then
        sign_one "$candidate"
    fi
done < <(find "$app/Contents" -type f -print)

nested_bundle_list="$temp_root/nested-bundles.txt"
python3 - "$app" "$nested_bundle_list" <<'PY'
import os
import pathlib
import sys

app = pathlib.Path(sys.argv[1]).resolve()
suffixes = {".app", ".appex", ".bundle", ".component", ".framework", ".plugin", ".xpc"}
bundles = []
for root, dirs, _files in os.walk(app / "Contents"):
    for name in dirs:
        path = pathlib.Path(root, name)
        if path.suffix.lower() in suffixes:
            bundles.append(path)
bundles.sort(key=lambda path: len(path.parts), reverse=True)
pathlib.Path(sys.argv[2]).write_text(
    "".join(f"{path}\n" for path in bundles),
    encoding="utf-8",
)
PY
while IFS= read -r nested_bundle; do
    [[ -n "$nested_bundle" ]] || continue
    sign_one "$nested_bundle"
done <"$nested_bundle_list"

codesign \
    --force \
    --sign "$signing_identity_sha1" \
    --keychain "$keychain_path" \
    --options runtime \
    --timestamp \
    --entitlements "$entitlements" \
    "$app"
require_signed_leaf_certificate "$app"

codesign --verify --deep --strict --verbose=4 "$app" \
    2>&1 | tee "$logs_dir/app-codesign-verify.txt"
codesign -dvvv "$app" 2>&1 | tee "$logs_dir/app-codesign-details.txt"
grep -Fq "Authority=$signing_identity" "$logs_dir/app-codesign-details.txt" \
    || fail "Authority do app inesperada"
grep -Fq "TeamIdentifier=$APPLE_TEAM_ID" "$logs_dir/app-codesign-details.txt" \
    || fail "TeamIdentifier do app inesperado"
grep -Fq "Timestamp=" "$logs_dir/app-codesign-details.txt" \
    || fail "timestamp seguro ausente na assinatura do app"
grep -Eq 'flags=.*runtime' "$logs_dir/app-codesign-details.txt" \
    || fail "Hardened Runtime ausente na assinatura do app"

signed_entitlements="$logs_dir/signed-entitlements.plist"
codesign -d --entitlements :- "$app" >"$signed_entitlements" 2>"$logs_dir/entitlements-codesign.txt"
plutil -lint "$signed_entitlements"
python3 - "$signed_entitlements" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as handle:
    actual = plistlib.load(handle)
expected = {
    "com.apple.security.cs.allow-jit": True,
    "com.apple.security.cs.allow-unsigned-executable-memory": True,
    "com.apple.security.cs.disable-library-validation": True,
    "com.apple.security.device.audio-input": True,
    "com.apple.security.device.camera": True,
}
if actual != expected:
    raise SystemExit(f"entitlements assinados divergentes: {actual!r}")
PY

validate_signed_macho() {
    local target="$1"
    local details="$temp_root/codesign-details.txt"
    codesign --verify --strict --verbose=2 "$target"
    codesign -dvvv "$target" >"$details" 2>&1
    grep -Fq "Authority=$signing_identity" "$details" \
        || fail "Mach-O sem a Authority esperada: $target"
    grep -Fq "TeamIdentifier=$APPLE_TEAM_ID" "$details" \
        || fail "Mach-O sem o TeamIdentifier esperado: $target"
    grep -Fq "Timestamp=" "$details" \
        || fail "Mach-O sem timestamp: $target"
    grep -Eq 'flags=.*runtime' "$details" \
        || fail "Mach-O sem Hardened Runtime: $target"
}

while IFS= read -r candidate; do
    if file -b "$candidate" | grep -q 'Mach-O'; then
        validate_signed_macho "$candidate"
    fi
done < <(find "$app/Contents" -type f -print)

app_zip="$temp_root/ChurchDAW-$release_version-app.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$app_zip"
[[ -s "$app_zip" ]] || fail "ZIP de notarizacao do app ficou vazio"

submit_and_require_acceptance() {
    local package="$1"
    local label="$2"
    local response="$logs_dir/${label}-notary-submit.json"
    local id_file="$temp_root/${label}-notary-id.txt"

    xcrun notarytool submit "$package" \
        --key "$notary_key_path" \
        --key-id "$APPLE_NOTARY_KEY_ID" \
        --issuer "$APPLE_NOTARY_ISSUER_ID" \
        --wait \
        --output-format json >"$response"

    python3 - "$response" "$id_file" <<'PY'
import json
import pathlib
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    response = json.load(handle)
status = response.get("status")
submission_id = response.get("id")
if status != "Accepted":
    raise SystemExit(f"notarizacao recusada ou inconclusiva: status={status!r}")
if not isinstance(submission_id, str) or not submission_id:
    raise SystemExit("notarytool nao retornou o submission id")
pathlib.Path(sys.argv[2]).write_text(submission_id, encoding="ascii")
PY

    local submission_id
    submission_id="$(cat "$id_file")"
    xcrun notarytool log "$submission_id" \
        --key "$notary_key_path" \
        --key-id "$APPLE_NOTARY_KEY_ID" \
        --issuer "$APPLE_NOTARY_ISSUER_ID" \
        --output-format json >"$logs_dir/${label}-notary-log.json"
    python3 - "$logs_dir/${label}-notary-log.json" "$label" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
label = sys.argv[2]

try:
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("BOM nao permitido")
    payload = json.loads(raw.decode("utf-8", errors="strict"))
except Exception as exc:
    raise SystemExit(f"log do notario {label} nao e JSON valido: {exc}")

if not isinstance(payload, dict):
    raise SystemExit(f"log do notario {label} nao e um objeto JSON")
if payload.get("status") != "Accepted":
    raise SystemExit(
        f"log do notario {label} nao confirma Accepted: "
        f"{payload.get('status')!r}"
    )

empty_values = (None, "", [], {})
for field in ("issues", "problems", "warnings"):
    value = payload.get(field)
    if value not in empty_values:
        raise SystemExit(
            f"log do notario {label} contem {field} nao vazio: {value!r}"
        )
PY
    printf '%s' "$submission_id"
}

app_submission_id="$(submit_and_require_acceptance "$app_zip" app)"
xcrun stapler staple "$app" | tee "$logs_dir/app-staple.txt"
xcrun stapler validate "$app" | tee "$logs_dir/app-staple-validate.txt"
codesign --verify --deep --strict --check-notarization --verbose=4 "$app" \
    2>&1 | tee "$logs_dir/app-post-notary-codesign.txt"
spctl --assess --type execute --verbose=4 "$app" \
    2>&1 | tee "$logs_dir/app-gatekeeper.txt"
grep -Fq "source=Notarized Developer ID" "$logs_dir/app-gatekeeper.txt" \
    || fail "Gatekeeper nao reconheceu o app como Notarized Developer ID"
grep -Fq "origin=$signing_identity" "$logs_dir/app-gatekeeper.txt" \
    || fail "Gatekeeper reportou origem inesperada para o app"

dmg_stage="$temp_root/dmg-root"
mkdir -p "$dmg_stage"
ditto "$app" "$dmg_stage/ChurchDAW.app"
ln -s /Applications "$dmg_stage/Applications"

dmg_name="ChurchDAW-$release_version-macOS.dmg"
dmg_path="$artifacts_dir/$dmg_name"
hdiutil create \
    -volname "ChurchDAW $release_version" \
    -srcfolder "$dmg_stage" \
    -fs HFS+ \
    -format UDZO \
    -imagekey zlib-level=9 \
    "$dmg_path" \
    2>&1 | tee "$logs_dir/hdiutil-create.txt"
[[ -s "$dmg_path" ]] || fail "DMG ficou vazio"

codesign \
    --force \
    --sign "$signing_identity_sha1" \
    --keychain "$keychain_path" \
    --timestamp \
    "$dmg_path"
require_signed_leaf_certificate "$dmg_path"
codesign --verify --strict --verbose=4 "$dmg_path" \
    2>&1 | tee "$logs_dir/dmg-codesign-verify.txt"
codesign -dvvv "$dmg_path" 2>&1 | tee "$logs_dir/dmg-codesign-details.txt"
grep -Fq "Authority=$signing_identity" "$logs_dir/dmg-codesign-details.txt" \
    || fail "Authority do DMG inesperada"
grep -Fq "TeamIdentifier=$APPLE_TEAM_ID" "$logs_dir/dmg-codesign-details.txt" \
    || fail "TeamIdentifier do DMG inesperado"
grep -Fq "Timestamp=" "$logs_dir/dmg-codesign-details.txt" \
    || fail "timestamp seguro ausente na assinatura do DMG"

dmg_submission_id="$(submit_and_require_acceptance "$dmg_path" dmg)"
xcrun stapler staple "$dmg_path" | tee "$logs_dir/dmg-staple.txt"
xcrun stapler validate "$dmg_path" | tee "$logs_dir/dmg-staple-validate.txt"
codesign --verify --strict --verbose=4 "$dmg_path" \
    2>&1 | tee "$logs_dir/dmg-post-staple-codesign.txt"
spctl --assess \
    --type open \
    --context context:primary-signature \
    --verbose=4 \
    "$dmg_path" \
    2>&1 | tee "$logs_dir/dmg-gatekeeper.txt"
grep -Fq "source=Notarized Developer ID" "$logs_dir/dmg-gatekeeper.txt" \
    || fail "Gatekeeper nao reconheceu o DMG como Notarized Developer ID"
grep -Fq "origin=$signing_identity" "$logs_dir/dmg-gatekeeper.txt" \
    || fail "Gatekeeper reportou origem inesperada para o DMG"

mount_dir="$(mktemp -d "$runner_temp/churchdaw-dmg-mount.XXXXXX")"
hdiutil attach "$dmg_path" \
    -readonly \
    -nobrowse \
    -mountpoint "$mount_dir" \
    2>&1 | tee "$logs_dir/hdiutil-attach.txt"
mounted_app="$mount_dir/ChurchDAW.app"
[[ -d "$mounted_app" ]] || fail "ChurchDAW.app ausente do DMG"
[[ -L "$mount_dir/Applications" ]] || fail "atalho Applications ausente do DMG"
[[ "$(readlink "$mount_dir/Applications")" == "/Applications" ]] \
    || fail "atalho Applications aponta para destino inesperado"
codesign --verify --deep --strict --check-notarization --verbose=4 "$mounted_app" \
    2>&1 | tee "$logs_dir/mounted-app-codesign.txt"
xcrun stapler validate "$mounted_app" \
    2>&1 | tee "$logs_dir/mounted-app-staple.txt"
spctl --assess --type execute --verbose=4 "$mounted_app" \
    2>&1 | tee "$logs_dir/mounted-app-gatekeeper.txt"
[[ "$(
    /usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
        "$mounted_app/Contents/Info.plist"
)" == "$EXPECTED_BUNDLE_ID" ]] || fail "bundle id mudou dentro do DMG"
[[ "$(
    /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$mounted_app/Contents/Info.plist"
)" == "$release_version" ]] || fail "versao mudou dentro do DMG"
[[ "$(
    /usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' \
        "$mounted_app/Contents/Info.plist"
)" == "$BUILD_NUMBER" ]] || fail "build number mudou dentro do DMG"

source_executable_sha="$(shasum -a 256 "$executable" | awk '{print $1}')"
mounted_executable_sha="$(
    shasum -a 256 "$mounted_app/Contents/MacOS/ChurchDAW" | awk '{print $1}'
)"
[[ "$source_executable_sha" == "$mounted_executable_sha" ]] \
    || fail "executavel dentro do DMG nao corresponde ao app notarizado"

hdiutil detach "$mount_dir" 2>&1 | tee "$logs_dir/hdiutil-detach.txt"
rmdir "$mount_dir"
mount_dir=""

dmg_sha256="$(shasum -a 256 "$dmg_path" | awk '{print $1}')"
dmg_bytes="$(stat -f '%z' "$dmg_path")"

(
    cd "$artifacts_dir"
    shasum -a 256 "$dmg_name" >SHA256SUMS.txt
)

export release_version BUILD_NUMBER RELEASE_TAG source_commit
export EXPECTED_BUNDLE_ID MACOSX_DEPLOYMENT_TARGET APPLE_TEAM_ID
export signing_identity certificate_sha256 dmg_name dmg_sha256 dmg_bytes
export app_submission_id dmg_submission_id source_executable_sha
python3 - "$artifacts_dir/release-manifest.json" <<'PY'
import json
import os
import pathlib

manifest = {
    "schema_version": 1,
    "product": "ChurchDAW",
    "platform": "macOS",
    "release_tag": os.environ["RELEASE_TAG"],
    "release_version": os.environ["release_version"],
    "build_number": int(os.environ["BUILD_NUMBER"]),
    "source_commit": os.environ["source_commit"],
    "bundle_id": os.environ["EXPECTED_BUNDLE_ID"],
    "deployment_target": os.environ["MACOSX_DEPLOYMENT_TARGET"],
    "architectures": ["arm64", "x86_64"],
    "team_id": os.environ["APPLE_TEAM_ID"],
    "signing_identity": os.environ["signing_identity"],
    "certificate_sha256": os.environ["certificate_sha256"],
    "app_executable_sha256": os.environ["source_executable_sha"],
    "app_notary_submission_id": os.environ["app_submission_id"],
    "dmg_notary_submission_id": os.environ["dmg_submission_id"],
    "artifact": {
        "name": os.environ["dmg_name"],
        "bytes": int(os.environ["dmg_bytes"]),
        "sha256": os.environ["dmg_sha256"],
    },
    "checks": {
        "developer_id_application": True,
        "secure_timestamp": True,
        "hardened_runtime": True,
        "entitlements_exact": True,
        "app_notarization_accepted": True,
        "app_ticket_stapled": True,
        "dmg_signed": True,
        "dmg_notarization_accepted": True,
        "dmg_ticket_stapled": True,
        "gatekeeper_app_accepted": True,
        "gatekeeper_dmg_accepted": True,
    },
}
pathlib.Path(__import__("sys").argv[1]).write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY
python3 -m json.tool "$artifacts_dir/release-manifest.json" >/dev/null

printf '\nRELEASE_MACOS_OK\n'
printf 'DMG: %s\n' "$dmg_path"
printf 'SHA-256: %s\n' "$dmg_sha256"
printf 'App notarization: %s\n' "$app_submission_id"
printf 'DMG notarization: %s\n' "$dmg_submission_id"
