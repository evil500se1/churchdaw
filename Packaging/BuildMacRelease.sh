#!/bin/bash
# Monta o .dmg do ChurchDAW para macOS.
#
# Irmao do BuildDesktopRelease.ps1 do Windows. Faz o mesmo papel: pega o app
# compilado, empacota no formato que o usuario final instala, e informa o
# SHA-256 e o tamanho que a policy de atualizacao precisa.
#
# ASSINATURA
# ----------
# Sem -i (Developer ID) o DMG sai NAO ASSINADO. Isso serve para testar, mas
# nao para distribuir: o Gatekeeper recusa, e o DesktopUpdater tambem -- ele
# confere a assinatura do sistema antes de executar qualquer instalador
# (ver Source/DesktopUpdater.h, "duas provas independentes").
#
# Com o certificado da Apple em maos, o fluxo completo e:
#   ./BuildMacRelease.sh -i "Developer ID Application: SEU NOME (TEAMID)" \
#                        -n "conta@exemplo.com" -t TEAMID
# que assina, notariza e grampeia o ticket.
#
# Uso simples (sem assinar):
#   ./BuildMacRelease.sh -b ~/churchdaw/build4

set -euo pipefail

BUILD_DIR=""
IDENTITY=""
NOTARY_PROFILE=""
APPLE_ID=""
TEAM_ID=""
OUTPUT_DIR=""
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
ENTITLEMENTS="$SCRIPT_DIR/ChurchDAW.entitlements"

while getopts "b:i:n:t:o:h" opt; do
    case "$opt" in
        b) BUILD_DIR="$OPTARG" ;;
        i) IDENTITY="$OPTARG" ;;
        n) APPLE_ID="$OPTARG" ;;
        t) TEAM_ID="$OPTARG" ;;
        o) OUTPUT_DIR="$OPTARG" ;;
        h) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "opcao invalida; use -h" >&2; exit 2 ;;
    esac
done

if [ -z "$BUILD_DIR" ]; then
    echo "erro: informe o diretorio de build com -b" >&2
    exit 2
fi

APP=$(find "$BUILD_DIR" -maxdepth 4 -name 'ChurchDAW.app' -type d | head -1)
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "erro: ChurchDAW.app nao encontrado em $BUILD_DIR" >&2
    exit 1
fi

PLIST="$APP/Contents/Info.plist"
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$PLIST")
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST")

if [ -z "$OUTPUT_DIR" ]; then
    OUTPUT_DIR="$(dirname "$BUILD_DIR")/dist"
fi
mkdir -p "$OUTPUT_DIR"

DMG="$OUTPUT_DIR/ChurchDAW-$VERSION.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "app        : $APP"
echo "versao     : $VERSION"
echo "bundle id  : $BUNDLE_ID"

#------------------------------------------------------------------ conferencias
# Estas checagens existem porque as duas falhas mais caras do macOS sao
# silenciosas: um binario que so roda na maquina que compilou, e um piso de
# sistema que exclui os clientes sem ninguem perceber.
ARCHS=$(lipo -archs "$APP/Contents/MacOS/ChurchDAW" 2>/dev/null || echo '?')
echo "arquiteturas: $ARCHS"
case "$ARCHS" in
    *x86_64*arm64*|*arm64*x86_64*) ;;
    *) echo "AVISO: binario nao e universal; Macs Intel ou Apple Silicon ficarao de fora" >&2 ;;
esac

MINOS=$(vtool -show-build "$APP/Contents/MacOS/ChurchDAW" 2>/dev/null \
        | awk '/minos/ {print $2; exit}')
echo "minos      : ${MINOS:-desconhecido}"
case "${MINOS:-0}" in
    11.*|12.*|13.*) ;;
    *) echo "AVISO: piso de sistema ${MINOS:-?} -- confira CMAKE_OSX_DEPLOYMENT_TARGET" >&2 ;;
esac

#--------------------------------------------------------------------- assinar
if [ -n "$IDENTITY" ]; then
    echo "== assinando o app =="
    if [ ! -s "$ENTITLEMENTS" ]; then
        echo "erro: entitlements ausente ou vazio: $ENTITLEMENTS" >&2
        exit 1
    fi
    plutil -lint "$ENTITLEMENTS" >/dev/null
    # --deep e desaconselhado pela Apple, mas o bundle do JUCE nao tem
    # frameworks aninhados; assinar o bundle inteiro basta.
    codesign --force --options runtime --timestamp \
             --entitlements "$ENTITLEMENTS" \
             --sign "$IDENTITY" "$APP"
    codesign --verify --deep --strict --verbose=2 "$APP"

    SIGNED_ENTITLEMENTS="$STAGE/signed-entitlements.plist"
    codesign -d --entitlements :- "$APP" >"$SIGNED_ENTITLEMENTS" 2>/dev/null
    plutil -lint "$SIGNED_ENTITLEMENTS" >/dev/null
    if [ "$(/usr/libexec/PlistBuddy -c \
             'Print :com.apple.security.cs.disable-library-validation' \
             "$SIGNED_ENTITLEMENTS" 2>/dev/null || true)" != "true" ]; then
        echo "erro: assinatura sem com.apple.security.cs.disable-library-validation=true" >&2
        exit 1
    fi
else
    echo "== SEM assinatura (build de teste) =="
fi

#----------------------------------------------------------------------- dmg
echo "== montando o dmg =="
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "ChurchDAW $VERSION" -srcfolder "$STAGE" \
               -ov -format UDZO "$DMG" >/dev/null

if [ -n "$IDENTITY" ]; then
    codesign --force --sign "$IDENTITY" --timestamp "$DMG"

    if [ -n "$APPLE_ID" ] && [ -n "$TEAM_ID" ]; then
        echo "== notarizando (pode levar alguns minutos) =="
        xcrun notarytool submit "$DMG" \
              --apple-id "$APPLE_ID" --team-id "$TEAM_ID" \
              --keychain-profile "${NOTARY_PROFILE:-churchdaw}" --wait
        # grampear o ticket deixa o DMG valido mesmo sem internet no cliente
        xcrun stapler staple "$DMG"
    else
        echo "AVISO: sem -n/-t, o DMG nao foi notarizado" >&2
    fi
fi

#-------------------------------------------------------------------- resumo
SHA=$(shasum -a 256 "$DMG" | awk '{print $1}')
BYTES=$(stat -f%z "$DMG")
ASSESS=$(spctl --assess --type install "$DMG" 2>&1 || true)

echo ""
echo "== resultado =="
echo "dmg        : $DMG"
echo "bytes      : $BYTES"
echo "sha256     : $SHA"
echo "gatekeeper : $ASSESS"
echo ""
echo "para a policy de atualizacao:"
echo "  mac_installer_url    https://churchdaw.com/app/desktop/$(basename "$DMG")"
echo "  mac_installer_sha256 $SHA"
echo "  mac_installer_bytes  $BYTES"

if [ -z "$IDENTITY" ]; then
    echo ""
    echo "LEMBRETE: sem assinatura este DMG serve para instalacao manual, mas a"
    echo "atualizacao obrigatoria vai recusa-lo e abrir na versao antiga."
fi
