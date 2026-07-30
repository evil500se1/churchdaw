#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Chave PUBLICA que autoriza uma atualizacao obrigatoria no desktop.
//
// E a MESMA chave do Android (ChurchDAWSignedPolicyVerifier.java, key_id
// "churchdaw-update-2026-01"). Deliberadamente a mesma: uma unica chave privada
// para guardar, um unico script de assinatura, e uma policy que nao pode ser
// valida em uma plataforma e invalida em outra.
//
// NAO confundir com a chave de LICENCA (DesktopLicensePublicKey.h, key_id
// "churchdaw-2026-01"). Sao papeis diferentes: aquela diz quem pode usar, esta
// diz qual versao pode rodar. Comprometer uma nao entrega a outra.
//
// Gerado de Packaging/AndroidUpdatePolicyPublicKey.pem -- nao editar a mao.
// A chave privada correspondente fica em
// %USERPROFILE%\ChurchDAW-Secrets\churchdaw-update-policy-private.pem,
// fora do repositorio e fora da hospedagem.
//==============================================================================
namespace ChurchDAWDesktopUpdate
{
// SubjectPublicKeyInfo DER (X.509), RSA-3072, em Base64 -- 422 bytes decodificados.
inline const char* const updatePolicyPublicKeyDerBase64 =
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAsOVhe3vxcydvPbdkGQMg"
    "53MFe5k7r+HFeMd4RRqWXdZIgAeF9RfMt3vFkX+AFgOHh6HsNR5CTA4UZXqtOBut"
    "I/oZibVjWC1C1F3EHAgSqjLwOhT58vs+ucTkdaS2zcdkB4fPOt80OM+yo4vINOl9"
    "ajh9ABDtcPk5p4LWMUiP9+eO+KsarJ7kB2XcerGYQzb1GtrmNAVLxq8wPsks/rhZ"
    "GjF38ctuhxbtPVwVzlLeyWl//kzW8uzFp/gSuO8yb3emaRk3k7OVZrx7YH8cDvSx"
    "F8E4tV+scd9l09l1Qi/a5hq9n9bxod8Yak4tdQzQuk1eM2F9HeNE+4C7OuVMXzTv"
    "1TPNezQydX5sowLwGX8LOUcM/BumKM9NMGEd8dOB5BHqJlA9KWThRANkjcudhYSr"
    "gJbRWvM9ZOgMknOl18+Q0F+/XWSjfMWzBzDdggmZY69MfgQXVGkW5X0RrS1o+GA3"
    "T/949ToRXGhcRzKmXgmiQwPl8b6K2oA4tO2l9w9X/Y7xAgMBAAE=";
}