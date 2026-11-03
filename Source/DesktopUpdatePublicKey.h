#pragma once

#include <juce_core/juce_core.h>

//==============================================================================
// Chave PUBLICA que autoriza uma atualizacao obrigatoria no desktop.
//
// E a mesma familia de chaves do Android (ChurchDAWSignedPolicyVerifier.java).
// O key_id assinado seleciona a chave 2026-01 ou a rotacao 2026-08; qualquer
// outro identificador e rejeitado.
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

// Rotacao 2026-08. A chave anterior acima continua valida para politicas
// antigas; o key_id assinado escolhe explicitamente qual chave usar.
inline const char* const updatePolicyPublicKeyDerBase64_2026_08 =
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAv5ZtiPbh+xhdk9POqWX7VQcCKzOcFXfxFLYUnFkClzc3ErmIVbkY4hBsbfHUwKhEiUdbCRTzNiOn7Y6hGX6iuN5FtPPqhcecXPbxWaSyP2x6Ukk2ThKov8Y9i1eo7y7zltVOlHVcDp8kqsQNobWUS93gONE33xha0uriF6Mhge5EHXGbEfBLSDLYojm46dqwDrQpE7VPfV1hPETTWBlU3oLqs1mAhkD5tAXEVPOfmkVR35ZxPyycdvUp62x2TwNY/sflHcDls0Jtg/SrH91sUJtUkY6wnWzK7RXlLCJyQ5aC1oPBICZSVb0n07xDj1Use3Rr7hQAi91cDL2GKQN1X3MJ7Go9NgENvPB19kSZDDQt62WRMF1fQNDVWtxDc0kMy6qVWwTv/jCc9X+YMyQ/M9ebkkYSjdgpioVms1jfBN/vMHn+qLalo1zL2EHjOS+JncHZhEbvxYsOAmRdvKZpxWntvdPCqvvlJg+r6+JuluYf3p29+vp7jKCbW2TeXjD3AgMBAAE=";

// Rotacao 2026-11 (update-policy-v3.json). As chaves 2026-01 e 2026-08 permanecem aceitas.
inline const char* const updatePolicyPublicKeyDerBase64_2026_11 =
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAtPuym7becfl2i5m2CB3AoDc4ydpGzYmmjDlEOUcyBHQK4QWXTeI8l9G9EHVBfSxDIDNBHGVMicVVyxof2w6aflo+cK9jFL5s4EZz60xfVSfYlOjeLjxWXmbxPAWl1HVEotJtP4DGFcwek5MBT2LV/mg82+9pa9oyzF6MiRsGdfg74dz9dxAPvpsnXAMIAyPIcgf5zAaIh+UkNwwhaPYD6ntOhmCfgE7t1ZBLv4FZvP/10SvqsyKlDEKbd8rr9/eAcxupWlZzepyI7MxRDF5/o30OXcPiMHfs1PVx7bd2w9Y53Nvx6CfkcJ503zQHyBOy+G4wQCrUNOkFLMa4sktIJnOuTt3fz5WgJi/3fTi7t6xwhs6G/kV0bWOIClW3zdwdq+mzyFl7bQxrAAn94c2WsBUSdSnT/PqyYAf0JUxuAk/0tDLqGp/phKPyTQZ/hG1O11hLBLUfunavMSkPosob+FviNpBjTrAbX0DMwakTGGN3clcf9cxn9D/h7oVgevKVAgMBAAE=";
}
