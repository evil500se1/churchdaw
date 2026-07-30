// Instalador do ChurchDAW para Windows.
//
// POR QUE ELE INSTALA "SO PARA O USUARIO"
// ---------------------------------------
// Instala em %LOCALAPPDATA%\Programs\ChurchDAW, nao em Arquivos de Programas.
// Isso e deliberado e vale mais do que a aparencia de "instalador profissional":
//
//   O ChurchDAW se atualiza sozinho e de forma obrigatoria. Num PC de igreja,
//   quem senta na frente do teclado quase nunca e o administrador da maquina.
//   Se a atualizacao obrigatoria abrisse um pedido de senha de administrador
//   que o musico nao tem, o portao viraria exatamente o tijolo que ele foi
//   desenhado para evitar -- um app que so sabe dizer "atualize" e nenhuma
//   forma de atualizar.
//
// Instalando no perfil do usuario, a atualizacao acontece sem UAC, sem senha e
// sem ninguem para chamar. E o mesmo motivo pelo qual o Chrome e o VS Code
// fazem assim.
//
// TROCA DE UM EXE QUE PODE ESTAR RODANDO
// --------------------------------------
// O Windows nao deixa sobrescrever nem apagar a imagem de um processo vivo, mas
// deixa RENOMEAR. Entao a troca e: renomeia o antigo para .old, poe o novo no
// lugar, e apaga o .old depois (ou na proxima execucao). Assim a instalacao
// funciona mesmo que o app ainda esteja fechando.
//
// Modos:
//   (sem argumentos)  instalacao interativa
//   /SILENT           sem nenhuma janela (usado pela atualizacao automatica)
//   /RELAUNCH         abre o ChurchDAW ao terminar
//   /UNINSTALL        desinstala

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>   // ShellExecuteW: WIN32_LEAN_AND_MEAN nao traz junto
#include <objbase.h>

#include <string>
#include <vector>

#ifndef CHURCHDAW_VERSION_NAME
 #define CHURCHDAW_VERSION_NAME L"0.0.0"
#endif

namespace
{

const wchar_t* const appExeName      = L"ChurchDAW.exe";
const wchar_t* const uninstallerName = L"Desinstalar ChurchDAW.exe";
const wchar_t* const displayName     = L"ChurchDAW";
const wchar_t* const publisher       = L"Igor Teixeira";
const wchar_t* const registryKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ChurchDAW";

bool silent   = false;
bool relaunch = false;

//==============================================================================
void report (const wchar_t* text, UINT icon = MB_ICONINFORMATION)
{
    if (! silent)
        MessageBoxW (nullptr, text, L"ChurchDAW", MB_OK | icon);
}

std::wstring knownFolder (REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    std::wstring result;

    if (SUCCEEDED (SHGetKnownFolderPath (id, 0, nullptr, &raw)) && raw != nullptr)
        result = raw;

    if (raw != nullptr)
        CoTaskMemFree (raw);

    return result;
}

std::wstring installFolder()
{
    auto base = knownFolder (FOLDERID_LocalAppData);

    if (base.empty())
        return {};

    return base + L"\\Programs\\ChurchDAW";
}

std::wstring thisExecutablePath()
{
    std::vector<wchar_t> buffer (MAX_PATH * 4, 0);
    const DWORD length = GetModuleFileNameW (nullptr, buffer.data(),
                                             (DWORD) buffer.size());
    return length > 0 ? std::wstring (buffer.data(), length) : std::wstring();
}

bool createFolders (const std::wstring& path)
{
    return SHCreateDirectoryExW (nullptr, path.c_str(), nullptr) == ERROR_SUCCESS
        || GetLastError() == ERROR_ALREADY_EXISTS
        || PathIsDirectoryW (path.c_str());
}

//==============================================================================
// O payload e o proprio ChurchDAW.exe, embutido como recurso RCDATA. Fica
// dentro da imagem PE, portanto entra no calculo da assinatura Authenticode --
// ao contrario de dados anexados no fim do arquivo, assinar cobre o payload.
bool writePayload (const std::wstring& destination)
{
    HRSRC found = FindResourceW (nullptr, MAKEINTRESOURCEW (1), RT_RCDATA);

    if (found == nullptr)
        return false;

    const DWORD size = SizeofResource (nullptr, found);
    HGLOBAL loaded = LoadResource (nullptr, found);

    if (size == 0 || loaded == nullptr)
        return false;

    const void* data = LockResource (loaded);

    if (data == nullptr)
        return false;

    HANDLE file = CreateFileW (destination.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const bool ok = WriteFile (file, data, size, &written, nullptr) != FALSE
                     && written == size;
    CloseHandle (file);

    if (! ok)
        DeleteFileW (destination.c_str());

    return ok;
}

// Troca o executavel mesmo que a versao antiga ainda esteja viva.
bool replaceExecutable (const std::wstring& target, const std::wstring& fresh)
{
    const std::wstring retired = target + L".old";

    DeleteFileW (retired.c_str());   // sobra de uma atualizacao anterior

    if (PathFileExistsW (target.c_str()))
    {
        // Renomear a imagem de um processo vivo E permitido; sobrescrever nao.
        if (! MoveFileExW (target.c_str(), retired.c_str(),
                           MOVEFILE_REPLACE_EXISTING))
            return false;
    }

    if (! MoveFileExW (fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        MoveFileExW (retired.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    // Pode falhar se o app ainda estiver fechando: some na proxima instalacao.
    DeleteFileW (retired.c_str());
    return true;
}

//==============================================================================
bool createShortcut (const std::wstring& linkPath,
                     const std::wstring& targetPath,
                     const std::wstring& description)
{
    IShellLinkW* link = nullptr;

    if (FAILED (CoCreateInstance (CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**) &link))
         || link == nullptr)
        return false;

    bool ok = false;
    link->SetPath (targetPath.c_str());
    link->SetDescription (description.c_str());

    const auto folder = targetPath.substr (0, targetPath.find_last_of (L'\\'));
    link->SetWorkingDirectory (folder.c_str());

    IPersistFile* persist = nullptr;
    if (SUCCEEDED (link->QueryInterface (IID_IPersistFile, (void**) &persist))
         && persist != nullptr)
    {
        ok = SUCCEEDED (persist->Save (linkPath.c_str(), TRUE));
        persist->Release();
    }

    link->Release();
    return ok;
}

std::wstring startMenuShortcutPath()
{
    const auto programs = knownFolder (FOLDERID_Programs);
    return programs.empty() ? std::wstring()
                            : programs + L"\\ChurchDAW.lnk";
}

std::wstring desktopShortcutPath()
{
    const auto desktop = knownFolder (FOLDERID_Desktop);
    return desktop.empty() ? std::wstring() : desktop + L"\\ChurchDAW.lnk";
}

//==============================================================================
void writeRegistryString (HKEY key, const wchar_t* name, const std::wstring& value)
{
    RegSetValueExW (key, name, 0, REG_SZ,
                    (const BYTE*) value.c_str(),
                    (DWORD) ((value.size() + 1) * sizeof (wchar_t)));
}

void writeUninstallEntry (const std::wstring& folder, DWORD sizeInKilobytes)
{
    HKEY key = nullptr;

    // HKEY_CURRENT_USER, nao LOCAL_MACHINE: instalacao por usuario nao precisa
    // (nem deveria) escrever na maquina inteira.
    if (RegCreateKeyExW (HKEY_CURRENT_USER, registryKey, 0, nullptr,
                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                         &key, nullptr) != ERROR_SUCCESS)
        return;

    const auto app = folder + L"\\" + appExeName;
    const auto uninstaller = L"\"" + folder + L"\\" + uninstallerName + L"\" /UNINSTALL";

    writeRegistryString (key, L"DisplayName", displayName);
    writeRegistryString (key, L"DisplayVersion", CHURCHDAW_VERSION_NAME);
    writeRegistryString (key, L"Publisher", publisher);
    writeRegistryString (key, L"InstallLocation", folder);
    writeRegistryString (key, L"DisplayIcon", app);
    writeRegistryString (key, L"UninstallString", uninstaller);

    const DWORD no = 1;
    RegSetValueExW (key, L"NoModify", 0, REG_DWORD, (const BYTE*) &no, sizeof (no));
    RegSetValueExW (key, L"NoRepair", 0, REG_DWORD, (const BYTE*) &no, sizeof (no));
    RegSetValueExW (key, L"EstimatedSize", 0, REG_DWORD,
                    (const BYTE*) &sizeInKilobytes, sizeof (sizeInKilobytes));

    RegCloseKey (key);
}

//==============================================================================
int install()
{
    const auto folder = installFolder();

    if (folder.empty() || ! createFolders (folder))
    {
        report (L"Nao foi possivel criar a pasta de instalacao.", MB_ICONERROR);
        return 1;
    }

    const auto target = folder + L"\\" + appExeName;
    const auto staging = folder + L"\\" + appExeName + L".new";

    if (! writePayload (staging))
    {
        report (L"Nao foi possivel gravar os arquivos do ChurchDAW.", MB_ICONERROR);
        return 1;
    }

    LARGE_INTEGER installedSize {};
    {
        HANDLE handle = CreateFileW (staging.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            GetFileSizeEx (handle, &installedSize);
            CloseHandle (handle);
        }
    }

    // A versao antiga pode ainda estar encerrando quando a atualizacao
    // automatica chama o instalador. Tentar por alguns segundos e mais honesto
    // do que falhar na primeira tentativa.
    bool replaced = false;
    for (int attempt = 0; attempt < 30 && ! replaced; ++attempt)
    {
        replaced = replaceExecutable (target, staging);

        if (! replaced)
            Sleep (500);
    }

    if (! replaced)
    {
        DeleteFileW (staging.c_str());
        report (L"O ChurchDAW parece estar aberto. Feche-o e tente de novo.",
                MB_ICONWARNING);
        return 1;
    }

    // O desinstalador e este mesmo executavel, guardado ao lado do app.
    CopyFileW (thisExecutablePath().c_str(),
               (folder + L"\\" + uninstallerName).c_str(), FALSE);

    const auto startMenu = startMenuShortcutPath();
    if (! startMenu.empty())
        createShortcut (startMenu, target, L"ChurchDAW");

    const auto desktop = desktopShortcutPath();
    if (! desktop.empty())
        createShortcut (desktop, target, L"ChurchDAW");

    writeUninstallEntry (folder, (DWORD) (installedSize.QuadPart / 1024));

    if (relaunch)
        ShellExecuteW (nullptr, L"open", target.c_str(), nullptr,
                       folder.c_str(), SW_SHOWNORMAL);

    report (L"ChurchDAW instalado.");
    return 0;
}

//==============================================================================
int uninstall()
{
    if (! silent)
    {
        const int answer = MessageBoxW (
            nullptr, L"Remover o ChurchDAW deste computador?", L"ChurchDAW",
            MB_YESNO | MB_ICONQUESTION);

        if (answer != IDYES)
            return 0;
    }

    const auto folder = installFolder();

    const auto startMenu = startMenuShortcutPath();
    if (! startMenu.empty())
        DeleteFileW (startMenu.c_str());

    const auto desktop = desktopShortcutPath();
    if (! desktop.empty())
        DeleteFileW (desktop.c_str());

    RegDeleteKeyW (HKEY_CURRENT_USER, registryKey);

    if (! folder.empty())
    {
        DeleteFileW ((folder + L"\\" + appExeName).c_str());
        DeleteFileW ((folder + L"\\" + appExeName + L".old").c_str());

        // O desinstalador nao consegue apagar a si mesmo enquanto roda: pede
        // ao Windows para remover no proximo reinicio.
        const auto self = thisExecutablePath();
        MoveFileExW (self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW (folder.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    report (L"ChurchDAW removido.");
    return 0;
}

}   // namespace

//==============================================================================
int WINAPI wWinMain (HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    const std::wstring arguments = commandLine != nullptr ? commandLine : L"";
    auto contains = [&arguments] (const wchar_t* flag)
    {
        return StrStrIW (arguments.c_str(), flag) != nullptr;
    };

    silent   = contains (L"/SILENT");
    relaunch = contains (L"/RELAUNCH");

    CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    const int result = contains (L"/UNINSTALL") ? uninstall() : install();
    CoUninitialize();

    return result;
}
