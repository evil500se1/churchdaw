#!/bin/bash

set -Eeuo pipefail
IFS=$'\n\t'
export LC_ALL=C

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

[[ $# -ge 5 && $# -le 8 ]] \
    || fail "uso: $0 ARTIFACT_ROOT RELEASE_TAG SOURCE_COMMIT EXPECTED_TEAM_ID EXPECTED_CERT_SHA256 [METADATA_JSON] [GITHUB_OUTPUT] [LOG_OUTPUT_DIR]"

artifact_root="$1"
release_tag="$2"
source_commit="$3"
expected_team_id="$4"
expected_certificate_sha256="$5"
metadata_output="${6:-}"
github_output="${7:-}"
log_output_dir="${8:-}"

[[ -d "$artifact_root" ]] || fail "raiz do artefato macOS ausente"
[[ "$release_tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    || fail "tag invalida: $release_tag"
[[ "$source_commit" =~ ^[0-9a-fA-F]{40}$ ]] \
    || fail "source commit invalido"
[[ "$expected_team_id" =~ ^[A-Z0-9]{10}$ ]] \
    || fail "Team ID esperado invalido"
[[ "$expected_certificate_sha256" =~ ^[0-9a-fA-F]{64}$ ]] \
    || fail "SHA-256 esperado do certificado invalido"

release_version="${release_tag#v}"
source_commit="$(printf '%s' "$3" | tr 'A-F' 'a-f')"
expected_certificate_sha256="$(
    printf '%s' "$expected_certificate_sha256" | tr 'A-F' 'a-f'
)"

resolve_unique() {
    local name="$1"
    local list_file="$temp_root/find-results.txt"
    find "$artifact_root" -type f -name "$name" -print >"$list_file"
    local count
    count="$(awk 'END { print NR + 0 }' "$list_file")"
    [[ "$count" -eq 1 ]] || fail "esperava um $name; encontrei $count"
    sed -n '1p' "$list_file"
}

runner_temp="${RUNNER_TEMP:-/tmp}"
temp_root="$(mktemp -d "$runner_temp/churchdaw-mac-verify.XXXXXX")"
mount_dir=""

cleanup() {
    local exit_code=$?
    trap - EXIT INT TERM
    if [[ -n "$mount_dir" && -d "$mount_dir" ]]; then
        hdiutil detach "$mount_dir" >/dev/null 2>&1 || true
        case "$mount_dir" in
            "$runner_temp"/churchdaw-mac-mount.*)
                rmdir "$mount_dir" >/dev/null 2>&1 || true
                ;;
            *)
                printf 'Mountpoint inesperado no cleanup: %s\n' \
                    "$mount_dir" >&2
                exit_code=1
                ;;
        esac
    fi
    case "$temp_root" in
        "$runner_temp"/churchdaw-mac-verify.*)
            rm -rf "$temp_root"
            ;;
        *)
            printf 'Recusando limpar temp_root inesperado: %s\n' \
                "$temp_root" >&2
            exit_code=1
            ;;
    esac
    exit "$exit_code"
}
trap cleanup EXIT INT TERM

dmg_name="ChurchDAW-$release_version-macOS.dmg"
manifest_path="$(resolve_unique release-manifest.json)"
checksums_path="$(resolve_unique SHA256SUMS.txt)"
dmg_path="$(resolve_unique "$dmg_name")"
artifact_dir="$(dirname "$manifest_path")"
[[ "$(dirname "$checksums_path")" == "$artifact_dir" ]] \
    || fail "SHA256SUMS fora do diretorio do manifest"
[[ "$(dirname "$dmg_path")" == "$artifact_dir" ]] \
    || fail "DMG fora do diretorio do manifest"

dmg_sha256="$(shasum -a 256 "$dmg_path" | awk '{ print $1 }')"
dmg_bytes="$(stat -f '%z' "$dmg_path")"
[[ "$dmg_sha256" =~ ^[0-9a-f]{64}$ ]]
[[ "$dmg_bytes" =~ ^[1-9][0-9]*$ ]]

python3 - "$manifest_path" "$checksums_path" "$dmg_name" \
    "$dmg_sha256" "$dmg_bytes" "$release_tag" "$release_version" \
    "$source_commit" "$expected_team_id" \
    "$expected_certificate_sha256" <<'PY'
import json
import pathlib
import re
import sys

(
    manifest_path,
    checksums_path,
    dmg_name,
    dmg_sha256,
    dmg_bytes,
    release_tag,
    release_version,
    source_commit,
    expected_team_id,
    expected_certificate_sha256,
) = sys.argv[1:]

def strict_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key: {key}")
        value[key] = item
    return value

try:
    manifest_raw = pathlib.Path(manifest_path).read_bytes()
    if manifest_raw.startswith(b"\xef\xbb\xbf"):
        raise ValueError("BOM nao permitido")
    manifest = json.loads(
        manifest_raw.decode("utf-8", errors="strict"),
        object_pairs_hook=strict_object,
    )
except Exception as exc:
    raise SystemExit(f"manifest macOS invalido: {exc}")

required_top = {
    "schema_version",
    "product",
    "platform",
    "release_tag",
    "release_version",
    "build_number",
    "source_commit",
    "bundle_id",
    "deployment_target",
    "architectures",
    "team_id",
    "signing_identity",
    "certificate_sha256",
    "app_executable_sha256",
    "app_notary_submission_id",
    "dmg_notary_submission_id",
    "artifact",
    "checks",
}
if set(manifest) != required_top:
    raise SystemExit(
        f"campos top-level inesperados: {sorted(set(manifest) ^ required_top)}"
    )

expected = {
    "schema_version": 1,
    "product": "ChurchDAW",
    "platform": "macOS",
    "release_tag": release_tag,
    "release_version": release_version,
    "source_commit": source_commit,
    "bundle_id": "com.churchdaw.app",
    "deployment_target": "10.15",
    "architectures": ["arm64", "x86_64"],
}
for name, value in expected.items():
    if manifest.get(name) != value:
        raise SystemExit(f"{name} diverge: {manifest.get(name)!r} != {value!r}")

if type(manifest["schema_version"]) is not int:
    raise SystemExit("schema_version precisa ser inteiro")
if type(manifest["build_number"]) is not int or manifest["build_number"] < 1:
    raise SystemExit("build_number invalido")
if manifest["team_id"] != expected_team_id:
    raise SystemExit("team_id diverge da configuracao protegida")
if not isinstance(manifest["signing_identity"], str) or any(
    ord(char) < 32 or ord(char) == 127
    for char in manifest["signing_identity"]
):
    raise SystemExit("signing_identity invalida ou contem controle")
if manifest["signing_identity"] != (
    f"Developer ID Application: "
    f"{manifest['signing_identity'].split('Developer ID Application: ', 1)[-1]}"
):
    raise SystemExit("signing_identity nao e Developer ID Application")
if not manifest["signing_identity"].endswith(f" ({manifest['team_id']})"):
    raise SystemExit("signing_identity nao termina no team_id")
for name in ("certificate_sha256", "app_executable_sha256"):
    if not isinstance(manifest[name], str) or not re.fullmatch(
        r"[0-9a-f]{64}", manifest[name]
    ):
        raise SystemExit(f"{name} invalido")
if manifest["certificate_sha256"] != expected_certificate_sha256:
    raise SystemExit("certificate_sha256 diverge da configuracao protegida")
for name in ("app_notary_submission_id", "dmg_notary_submission_id"):
    if not re.fullmatch(
        r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
        r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}",
        manifest[name],
    ):
        raise SystemExit(f"{name} invalido")

artifact = manifest["artifact"]
if set(artifact) != {"name", "bytes", "sha256"}:
    raise SystemExit("artifact macOS tem campos inesperados")
if type(artifact.get("bytes")) is not int or artifact != {
    "name": dmg_name,
    "bytes": int(dmg_bytes),
    "sha256": dmg_sha256,
}:
    raise SystemExit(f"artifact macOS diverge: {artifact!r}")

required_checks = {
    "developer_id_application",
    "secure_timestamp",
    "hardened_runtime",
    "entitlements_exact",
    "app_notarization_accepted",
    "app_ticket_stapled",
    "dmg_signed",
    "dmg_notarization_accepted",
    "dmg_ticket_stapled",
    "gatekeeper_app_accepted",
    "gatekeeper_dmg_accepted",
}
if set(manifest["checks"]) != required_checks:
    raise SystemExit("conjunto de checks macOS inesperado")
if any(value is not True for value in manifest["checks"].values()):
    raise SystemExit("manifest macOS contem check nao aprovado")

checksum_lines = pathlib.Path(checksums_path).read_text(
    encoding="utf-8", errors="strict"
).splitlines()
if checksum_lines != [f"{dmg_sha256}  {dmg_name}"]:
    raise SystemExit(f"SHA256SUMS macOS inesperado: {checksum_lines!r}")
PY

manifest_sha256="$(shasum -a 256 "$manifest_path" | awk '{ print $1 }')"
team_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["team_id"])' "$manifest_path")"
signing_identity="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["signing_identity"])' "$manifest_path")"
build_number="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["build_number"])' "$manifest_path")"

codesign --verify --strict --verbose=4 "$dmg_path" 2>&1 \
    | tee "$temp_root/dmg-codesign-verify.txt"
codesign -dvvv "$dmg_path" >"$temp_root/dmg-codesign-details.txt" 2>&1
grep -Fq "Authority=$signing_identity" "$temp_root/dmg-codesign-details.txt" \
    || fail "Authority do DMG diverge do manifest"
grep -Fq "TeamIdentifier=$team_id" "$temp_root/dmg-codesign-details.txt" \
    || fail "TeamIdentifier do DMG diverge do manifest"
grep -Fq 'Timestamp=' "$temp_root/dmg-codesign-details.txt" \
    || fail "timestamp seguro ausente no DMG"
xcrun stapler validate "$dmg_path" 2>&1 | tee "$temp_root/dmg-stapler.txt"
spctl --assess --type open --context context:primary-signature \
    --verbose=4 "$dmg_path" 2>&1 | tee "$temp_root/dmg-gatekeeper.txt"
grep -Fq 'source=Notarized Developer ID' "$temp_root/dmg-gatekeeper.txt" \
    || fail "Gatekeeper nao reconheceu DMG notarizado"
grep -Fq "origin=$signing_identity" "$temp_root/dmg-gatekeeper.txt" \
    || fail "origem Gatekeeper do DMG diverge"

mount_dir="$(mktemp -d "$runner_temp/churchdaw-mac-mount.XXXXXX")"
hdiutil attach "$dmg_path" -readonly -nobrowse -mountpoint "$mount_dir" \
    2>&1 | tee "$temp_root/hdiutil-attach.txt"
mounted_app="$mount_dir/ChurchDAW.app"
mounted_executable="$mounted_app/Contents/MacOS/ChurchDAW"
mounted_plist="$mounted_app/Contents/Info.plist"
[[ -d "$mounted_app" && -x "$mounted_executable" && -s "$mounted_plist" ]] \
    || fail "ChurchDAW.app valido ausente do DMG"
topology="$(
    find "$mount_dir" -mindepth 1 -maxdepth 1 -print \
        | sed 's#.*/##' \
        | LC_ALL=C sort
)"
[[ "$topology" == $'Applications\nChurchDAW.app' ]] \
    || fail "topologia inesperada no DMG: $topology"
[[ -L "$mount_dir/Applications" ]] \
    || fail "atalho Applications ausente do DMG"
[[ "$(readlink "$mount_dir/Applications")" == '/Applications' ]] \
    || fail "atalho Applications aponta para destino inesperado"

codesign --verify --deep --strict --check-notarization --verbose=4 \
    "$mounted_app" 2>&1 | tee "$temp_root/app-codesign.txt"
codesign -dvvv "$mounted_app" >"$temp_root/app-codesign-details.txt" 2>&1
grep -Fq "Authority=$signing_identity" "$temp_root/app-codesign-details.txt" \
    || fail "Authority do app diverge do manifest"
grep -Fq "TeamIdentifier=$team_id" "$temp_root/app-codesign-details.txt" \
    || fail "TeamIdentifier do app diverge do manifest"
grep -Fq 'Timestamp=' "$temp_root/app-codesign-details.txt" \
    || fail "timestamp seguro ausente no app"
grep -Eq 'flags=.*runtime' "$temp_root/app-codesign-details.txt" \
    || fail "Hardened Runtime ausente no app"
xcrun stapler validate "$mounted_app" 2>&1 | tee "$temp_root/app-stapler.txt"
spctl --assess --type execute --verbose=4 "$mounted_app" 2>&1 \
    | tee "$temp_root/app-gatekeeper.txt"
grep -Fq 'source=Notarized Developer ID' "$temp_root/app-gatekeeper.txt" \
    || fail "Gatekeeper nao reconheceu app notarizado"
grep -Fq "origin=$signing_identity" "$temp_root/app-gatekeeper.txt" \
    || fail "origem Gatekeeper do app diverge"

manifest_app_sha256="$(
    python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["app_executable_sha256"])' \
        "$manifest_path"
)"
mounted_app_sha256="$(shasum -a 256 "$mounted_executable" | awk '{ print $1 }')"
[[ "$mounted_app_sha256" == "$manifest_app_sha256" ]] \
    || fail "SHA-256 do executavel montado diverge do manifest"

(
    cd "$temp_root"
    codesign -d --extract-certificates "$mounted_app" >/dev/null 2>&1
)
leaf_certificate="$temp_root/codesign0"
[[ -s "$leaf_certificate" ]] || fail "certificado leaf nao foi extraido do app"
actual_certificate_sha256="$(shasum -a 256 "$leaf_certificate" | awk '{ print $1 }')"
[[ "$actual_certificate_sha256" == "$expected_certificate_sha256" ]] \
    || fail "certificado leaf diverge da configuracao protegida"

macho_index=0
while IFS= read -r -d '' candidate; do
    if file -b "$candidate" | grep -q 'Mach-O'; then
        ((macho_index += 1))
        macho_details="$temp_root/macho-$macho_index-details.txt"
        macho_dependencies="$temp_root/macho-$macho_index-dependencies.txt"
        codesign --verify --strict --verbose=2 "$candidate"
        codesign -dvvv "$candidate" >"$macho_details" 2>&1
        grep -Fq "Authority=$signing_identity" "$macho_details" \
            || fail "Mach-O com Authority divergente: $candidate"
        grep -Fq "TeamIdentifier=$team_id" "$macho_details" \
            || fail "Mach-O com TeamIdentifier divergente: $candidate"
        grep -Fq 'Timestamp=' "$macho_details" \
            || fail "Mach-O sem timestamp seguro: $candidate"
        grep -Eq 'flags=.*runtime' "$macho_details" \
            || fail "Mach-O sem Hardened Runtime: $candidate"

        macho_cert_dir="$(mktemp -d "$temp_root/macho-cert.XXXXXX")"
        (
            cd "$macho_cert_dir"
            codesign -d --extract-certificates "$candidate" >/dev/null 2>&1
        )
        [[ -s "$macho_cert_dir/codesign0" ]] \
            || fail "certificado leaf ausente no Mach-O: $candidate"
        macho_cert_sha256="$(
            shasum -a 256 "$macho_cert_dir/codesign0" | awk '{ print $1 }'
        )"
        [[ "$macho_cert_sha256" == "$expected_certificate_sha256" ]] \
            || fail "certificado leaf divergente no Mach-O: $candidate"

        otool -L "$candidate" >"$macho_dependencies"
        if grep -Eq '/Users/|/private/var/folders/|/tmp/|build-mac-commercial' \
                "$macho_dependencies"; then
            fail "dependencia do ambiente de build no Mach-O: $candidate"
        fi
    fi
done < <(find "$mounted_app/Contents" -type f -print0)
[[ "$macho_index" -ge 1 ]] || fail "nenhum Mach-O encontrado no app"

plist_read() {
    /usr/libexec/PlistBuddy -c "Print :$1" "$mounted_plist"
}
[[ "$(plist_read CFBundleIdentifier)" == 'com.churchdaw.app' ]] \
    || fail "bundle id inesperado no DMG"
[[ "$(plist_read CFBundleExecutable)" == 'ChurchDAW' ]] \
    || fail "CFBundleExecutable inesperado"
[[ "$(plist_read CFBundlePackageType)" == 'APPL' ]] \
    || fail "CFBundlePackageType inesperado"
[[ "$(plist_read CFBundleShortVersionString)" == "$release_version" ]] \
    || fail "versao do app diverge da tag"
[[ "$(plist_read CFBundleVersion)" == "$build_number" ]] \
    || fail "build number do app diverge do manifest"
[[ "$(plist_read LSMinimumSystemVersion)" == '10.15' ]] \
    || fail "piso macOS geral inesperado"
[[ "$(plist_read LSMinimumSystemVersionByArchitecture:x86_64)" == '10.15' ]] \
    || fail "piso x86_64 nao atende Catalina"
[[ "$(plist_read LSMinimumSystemVersionByArchitecture:arm64)" == '11.0' ]] \
    || fail "piso arm64 inesperado"
[[ -n "$(plist_read NSMicrophoneUsageDescription)" ]] \
    || fail "NSMicrophoneUsageDescription vazio"
[[ -n "$(plist_read NSBluetoothAlwaysUsageDescription)" ]] \
    || fail "NSBluetoothAlwaysUsageDescription vazio"

otool -L "$mounted_executable" | tee "$temp_root/macho-dependencies.txt"
if grep -Eq '/Users/|build-mac-commercial' "$temp_root/macho-dependencies.txt"; then
    fail "dependencia absoluta do ambiente de build encontrada"
fi

architectures="$(lipo -archs "$mounted_executable")"
[[ "$architectures" == 'x86_64 arm64' || "$architectures" == 'arm64 x86_64' ]] \
    || fail "slices inesperados: $architectures"
minos_dir="$temp_root/minos"
mkdir "$minos_dir"
lipo "$mounted_executable" -thin x86_64 -output "$minos_dir/ChurchDAW-x86_64"
lipo "$mounted_executable" -thin arm64 -output "$minos_dir/ChurchDAW-arm64"

require_minos() {
    local binary="$1"
    local expected="$2"
    local label="$3"
    local actual
    local platform
    actual="$(vtool -show-build "$binary" | awk '$1 == "minos" { print $2; exit }')"
    platform="$(vtool -show-build "$binary" | awk '$1 == "platform" { print $2; exit }')"
    [[ "$actual" == "$expected" && "$platform" == 'MACOS' ]] \
        || fail "$label minos/plataforma inesperado: $actual/$platform"
}
require_minos "$minos_dir/ChurchDAW-x86_64" '10.15' x86_64
require_minos "$minos_dir/ChurchDAW-arm64" '11.0' arm64

signed_entitlements="$temp_root/signed-entitlements.plist"
codesign -d --entitlements :- "$mounted_app" >"$signed_entitlements" \
    2>"$temp_root/entitlements-codesign.txt"
plutil -lint "$signed_entitlements"
python3 - "$signed_entitlements" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as handle:
    actual = plistlib.load(handle)
expected = {
    "com.apple.security.cs.disable-library-validation": True,
    "com.apple.security.device.audio-input": True,
}
if actual != expected:
    raise SystemExit(f"entitlements assinados divergentes: {actual!r}")
PY

hdiutil detach "$mount_dir" 2>&1 | tee "$temp_root/hdiutil-detach.txt"
rmdir "$mount_dir"
mount_dir=""

if [[ -n "$log_output_dir" ]]; then
    mkdir -p "$log_output_dir"
    find "$temp_root" -maxdepth 1 -type f \
        \( -name '*.txt' -o -name '*.plist' \) \
        -exec cp {} "$log_output_dir"/ \;
fi

if [[ -n "$metadata_output" ]]; then
    mkdir -p "$(dirname "$metadata_output")"
    python3 - "$metadata_output" "$release_tag" "$release_version" \
        "$source_commit" "$dmg_name" "$dmg_path" "$dmg_bytes" \
        "$dmg_sha256" "$manifest_sha256" "$team_id" \
        "$actual_certificate_sha256" <<'PY'
import json
import pathlib
import sys

(
    output,
    release_tag,
    release_version,
    source_commit,
    dmg_name,
    dmg_path,
    dmg_bytes,
    dmg_sha256,
    manifest_sha256,
    team_id,
    certificate_sha256,
) = sys.argv[1:]
value = {
    "schema_version": 1,
    "platform": "macOS",
    "release_tag": release_tag,
    "release_version": release_version,
    "source_commit": source_commit,
    "installer_name": dmg_name,
    "installer_path": dmg_path,
    "installer_bytes": int(dmg_bytes),
    "installer_sha256": dmg_sha256,
    "manifest_sha256": manifest_sha256,
    "team_id": team_id,
    "certificate_sha256": certificate_sha256,
}
pathlib.Path(output).write_text(
    json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY
fi

if [[ -n "$github_output" ]]; then
    {
        printf 'installer_sha256=%s\n' "$dmg_sha256"
        printf 'installer_bytes=%s\n' "$dmg_bytes"
        printf 'manifest_sha256=%s\n' "$manifest_sha256"
        printf 'team_id=%s\n' "$team_id"
        printf 'certificate_sha256=%s\n' "$actual_certificate_sha256"
    } >>"$github_output"
fi

printf 'VERIFY_MAC_RELEASE_ARTIFACT_OK\n'
printf 'DMG: %s\n' "$dmg_path"
printf 'SHA-256: %s\n' "$dmg_sha256"
printf 'Bytes: %s\n' "$dmg_bytes"
