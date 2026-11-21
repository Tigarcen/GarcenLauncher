// mingw-w64's windows.foundation.h declares both IReference<BYTE> and
// IReference<boolean> (boolean is a typedef of unsigned char), which the
// compiler sees as the same specialization. Skip the duplicate one.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "ControlConfig.h"

#include <cstdio>
#include <string>
#include <vector>

#include <roapi.h>
#include <winstring.h>
#include <windows.data.json.h>

// ========================================
// Default config file content
// ========================================

constexpr wchar_t defaultConfigString[] =
    L"{\r\n"
    L"    \"width\": 508,\r\n"
    L"    \"height\": 434,\r\n"
    L"    \"radius\": 10,\r\n"
    L"    \"theme\": \"dark\",\r\n"
    L"    \"opacity\": 1,\r\n"
    L"    \"hotkey_modifiers\": 1,\r\n"
    L"    \"hotkey_key\": 192\r\n"
    L"}\r\n";

// ========================================
// GUIDs of the Windows.Data.Json interfaces
// (declared locally so no import library symbol is needed)
// ========================================

static const GUID IID_JSON_VALUE_STATICS =
    {0x5f6b544a, 0x2f53, 0x48e1, {0x91, 0xa3, 0xf7, 0x8b, 0x50, 0xa6, 0x34, 0x5c}};

// ========================================
// Minimal RAII helpers
// ========================================

template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* Get() const { return m_ptr; }
    T* operator->() const { return m_ptr; }

    T** ReleaseAndGetAddressOf() { Reset(); return &m_ptr; }
    T** operator&() { Reset(); return &m_ptr; }

    void Reset()
    {
        if (m_ptr)
        {
            m_ptr->Release();
            m_ptr = nullptr;
        }
    }

private:
    T* m_ptr = nullptr;
};

class ScopedHString
{
public:
    ScopedHString() = default;
    ~ScopedHString() { Reset(); }

    ScopedHString(const ScopedHString&) = delete;
    ScopedHString& operator=(const ScopedHString&) = delete;

    bool Create(const wchar_t* text, size_t length)
    {
        Reset();
        return SUCCEEDED(WindowsCreateString(
            text,
            static_cast<UINT32>(length),
            &m_value));
    }

    HSTRING Get() const { return m_value; }

    void Reset()
    {
        if (m_value)
        {
            WindowsDeleteString(m_value);
            m_value = nullptr;
        }
    }

private:
    HSTRING m_value = nullptr;
};

// ========================================
// File helpers
// ========================================

static std::wstring GetConfigPath()
{
    wchar_t exePath[MAX_PATH] = {};

    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return L"config.gcson";

    std::wstring path(exePath);

    const size_t slash = path.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        path.resize(slash + 1);

    // JSON content, but the launcher's own extension so it is not opened
    // by JSON editors by default
    path += L"config.gcson";

    return path;
}

static bool WriteTextFile(const std::wstring& path, const wchar_t* text)
{
    // Encode the content as UTF-8
    const int wideLength = static_cast<int>(wcslen(text));

    int utf8Length = WideCharToMultiByte(
        CP_UTF8, 0, text, wideLength,
        nullptr, 0, nullptr, nullptr);

    if (utf8Length <= 0)
        return false;

    std::vector<char> utf8(static_cast<size_t>(utf8Length));

    if (WideCharToMultiByte(
        CP_UTF8, 0, text, wideLength,
        utf8.data(), utf8Length, nullptr, nullptr) != utf8Length)
    {
        return false;
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;

    const BOOL ok = WriteFile(
        file,
        utf8.data(),
        static_cast<DWORD>(utf8.size()),
        &written,
        nullptr);

    CloseHandle(file);

    return ok && written == utf8.size();
}

static bool WriteConfigFile(const std::wstring& path)
{
    return WriteTextFile(path, defaultConfigString);
}

static bool ReadConfigFile(const std::wstring& path, std::wstring& text)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return false;

    DWORD size = GetFileSize(file, nullptr);

    if (size == INVALID_FILE_SIZE || size == 0)
    {
        CloseHandle(file);
        return false;
    }

    std::vector<char> buffer(static_cast<size_t>(size));
    DWORD read = 0;

    const BOOL ok = ReadFile(file, buffer.data(), size, &read, nullptr);

    CloseHandle(file);

    if (!ok || read == 0)
        return false;

    // Skip the UTF-8 BOM if present
    DWORD offset = 0;

    if (read >= 3 &&
        static_cast<unsigned char>(buffer[0]) == 0xEF &&
        static_cast<unsigned char>(buffer[1]) == 0xBB &&
        static_cast<unsigned char>(buffer[2]) == 0xBF)
    {
        offset = 3;
    }

    int wideLength = MultiByteToWideChar(
        CP_UTF8, 0, buffer.data() + offset, static_cast<int>(read - offset),
        nullptr, 0);

    if (wideLength <= 0)
        return false;

    text.assign(static_cast<size_t>(wideLength), L'\0');

    MultiByteToWideChar(
        CP_UTF8, 0, buffer.data() + offset, static_cast<int>(read - offset),
        &text[0], wideLength);

    return true;
}

// ========================================
// JSON helpers
// ========================================

static bool TryGetNumber(
    ABI::Windows::Data::Json::IJsonObject* object,
    const wchar_t* name,
    double& result)
{
    ScopedHString key;

    if (!key.Create(name, wcslen(name)))
        return false;

    ComPtr<ABI::Windows::Data::Json::IJsonValue> value;

    if (FAILED(object->GetNamedValue(key.Get(), &value)))
        return false;

    double number = 0;

    if (FAILED(value->GetNumber(&number)))
        return false;

    result = number;
    return true;
}

static bool TryGetString(
    ABI::Windows::Data::Json::IJsonObject* object,
    const wchar_t* name,
    std::wstring& result)
{
    ScopedHString key;

    if (!key.Create(name, wcslen(name)))
        return false;

    ComPtr<ABI::Windows::Data::Json::IJsonValue> value;

    if (FAILED(object->GetNamedValue(key.Get(), &value)))
        return false;

    HSTRING text = nullptr;

    if (FAILED(value->GetString(&text)) || !text)
        return false;

    UINT32 length = 0;
    PCWSTR raw = WindowsGetStringRawBuffer(text, &length);

    if (raw)
        result.assign(raw, length);

    WindowsDeleteString(text);

    return raw != nullptr;
}

static void ParseConfig(const std::wstring& text, LauncherConfig& config)
{
    ScopedHString className;
    ScopedHString json;

    if (!className.Create(
        RuntimeClass_Windows_Data_Json_JsonValue,
        wcslen(RuntimeClass_Windows_Data_Json_JsonValue)))
    {
        return;
    }

    ComPtr<ABI::Windows::Data::Json::IJsonValueStatics> statics;

    HRESULT hr = RoGetActivationFactory(
        className.Get(),
        IID_JSON_VALUE_STATICS,
        reinterpret_cast<void**>(statics.ReleaseAndGetAddressOf()));

    if (FAILED(hr))
        return;

    if (!json.Create(text.c_str(), text.size()))
        return;

    ComPtr<ABI::Windows::Data::Json::IJsonValue> value;

    if (FAILED(statics->Parse(json.Get(), value.ReleaseAndGetAddressOf())))
        return;

    ComPtr<ABI::Windows::Data::Json::IJsonObject> object;

    if (FAILED(value->GetObject(object.ReleaseAndGetAddressOf())))
        return;

    double number = 0;

    if (TryGetNumber(object.Get(), L"width", number))
        config.width = number < 1 ? 1 : static_cast<int>(number);

    if (TryGetNumber(object.Get(), L"height", number))
        config.height = number < 1 ? 1 : static_cast<int>(number);

    if (TryGetNumber(object.Get(), L"radius", number))
        config.radius = number < 0 ? 0 : static_cast<int>(number);

    std::wstring theme;

    if (TryGetString(object.Get(), L"theme", theme) && !theme.empty())
    {
        const size_t length = theme.size() < 15 ? theme.size() : 15;

        memcpy(config.theme, theme.c_str(), length * sizeof(wchar_t));
        config.theme[length] = L'\0';
    }

    if (TryGetNumber(object.Get(), L"opacity", number))
    {
        if (number < 0.0)
            config.opacity = 0.0;
        else if (number > 1.0)
            config.opacity = 1.0;
        else
            config.opacity = number;
    }

    // Hotkey: MOD_* flags (only the four low bits are meaningful) and a
    // virtual-key code in the 1..254 range
    if (TryGetNumber(object.Get(), L"hotkey_modifiers", number))
    {
        if (number >= 0 && number <= 15)
            config.hotkeyModifiers = static_cast<UINT>(number);
    }

    if (TryGetNumber(object.Get(), L"hotkey_key", number))
    {
        if (number >= 1 && number <= 254)
            config.hotkeyKey = static_cast<UINT>(number);
    }
}

// ========================================
// Public API
// ========================================

LauncherConfig InitConfig()
{
    LauncherConfig config;

    // STA: the UI thread also hosts the OLE drop target (OleInitialize),
    // and initializing it as MTA first would block that.
    RoInitialize(RO_INIT_SINGLETHREADED);

    const std::wstring path = GetConfigPath();

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        WriteConfigFile(path);
    }

    std::wstring text;

    if (ReadConfigFile(path, text))
    {
        ParseConfig(text, config);
    }

    return config;
}

COLORREF GetThemeBackgroundColor(const wchar_t* theme)
{
    if (theme && _wcsicmp(theme, L"light") == 0)
        return RGB(0xff, 0xff, 0xff);

    return RGB(0x22, 0x26, 0x2a);
}

bool UpdateSizeLocalize(const LauncherConfig& config)
{
    wchar_t content[512] = {};

    const int length = swprintf(
        content,
        ARRAYSIZE(content),
        L"{\r\n"
        L"    \"width\": %d,\r\n"
        L"    \"height\": %d,\r\n"
        L"    \"radius\": %d,\r\n"
        L"    \"theme\": \"%ls\",\r\n"
        L"    \"opacity\": %g,\r\n"
        L"    \"hotkey_modifiers\": %u,\r\n"
        L"    \"hotkey_key\": %u\r\n"
        L"}\r\n",
        config.width,
        config.height,
        config.radius,
        config.theme,
        config.opacity,
        config.hotkeyModifiers,
        config.hotkeyKey);

    if (length <= 0)
        return false;

    return WriteTextFile(GetConfigPath(), content);
}

bool UpdateTheme(const LauncherConfig& config)
{
    return UpdateSizeLocalize(config);
}
