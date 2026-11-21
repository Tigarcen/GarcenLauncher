// mingw-w64's windows.foundation.h declares both IReference<BYTE> and
// IReference<boolean> (boolean is a typedef of unsigned char), which the
// compiler sees as the same specialization. Skip the duplicate one.
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include "List.h"

#include <cstdio>
#include <string>
#include <vector>

#include <roapi.h>
#include <winstring.h>
#include <windows.data.json.h>

// ========================================
// Default list file content (empty JSON array)
// ========================================

constexpr wchar_t defaultListString[] = L"[]\r\n";

// ========================================
// GUIDs of the Windows.Data.Json interfaces
// ========================================

static const GUID IID_JSON_VALUE_STATICS =
    {0x5f6b544a, 0x2f53, 0x48e1, {0x91, 0xa3, 0xf7, 0x8b, 0x50, 0xa6, 0x34, 0x5c}};

// ========================================
// Minimal RAII helpers (mirrored from ControlConfig.cpp)
// ========================================

template <typename T>
class ListComPtr
{
public:
    ListComPtr() = default;
    ~ListComPtr() { Reset(); }

    ListComPtr(const ListComPtr&) = delete;
    ListComPtr& operator=(const ListComPtr&) = delete;

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

class ListScopedHString
{
public:
    ListScopedHString() = default;
    ~ListScopedHString() { Reset(); }

    ListScopedHString(const ListScopedHString&) = delete;
    ListScopedHString& operator=(const ListScopedHString&) = delete;

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

static std::wstring GetListPath()
{
    wchar_t exePath[MAX_PATH] = {};

    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
        return L"list.json";

    std::wstring path(exePath);

    const size_t slash = path.find_last_of(L"\\/");

    if (slash != std::wstring::npos)
        path.resize(slash + 1);

    path += L"list.json";

    return path;
}

static bool WriteTextFile(const std::wstring& path, const wchar_t* text)
{
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

static bool ReadTextFile(const std::wstring& path, std::wstring& text)
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

static bool TryGetString(
    ABI::Windows::Data::Json::IJsonObject* object,
    const wchar_t* name,
    std::wstring& result)
{
    ListScopedHString key;

    if (!key.Create(name, wcslen(name)))
        return false;

    ListComPtr<ABI::Windows::Data::Json::IJsonValue> value;

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

static bool ParseItem(
    ABI::Windows::Data::Json::IJsonObject* object,
    Item& item);

static bool ParseItemArray(
    ABI::Windows::Data::Json::IJsonArray* array,
    std::vector<Item>& items)
{
    UINT32 i = 0;

    while (true)
    {
        ListComPtr<ABI::Windows::Data::Json::IJsonObject> subObject;

        const HRESULT hr = array->GetObjectAt(i, &subObject);

        if (FAILED(hr))
            break;

        Item child;

        if (ParseItem(subObject.Get(), child))
            items.push_back(std::move(child));

        ++i;
    }

    return true;
}

static bool ParseItem(
    ABI::Windows::Data::Json::IJsonObject* object,
    Item& item)
{
    TryGetString(object, L"name", item.name);
    TryGetString(object, L"path", item.path);

    ListScopedHString childrenKey;

    if (childrenKey.Create(L"children", 8))
    {
        ListComPtr<ABI::Windows::Data::Json::IJsonValue> childrenValue;

        if (SUCCEEDED(object->GetNamedValue(childrenKey.Get(), &childrenValue)))
        {
            ListComPtr<ABI::Windows::Data::Json::IJsonArray> childrenArray;

            if (SUCCEEDED(childrenValue->GetArray(&childrenArray)))
            {
                ParseItemArray(childrenArray.Get(), item.children);
            }
        }
    }

    return true;
}

static std::vector<Item> ParseListJson(const std::wstring& text)
{
    std::vector<Item> items;

    ListScopedHString className;
    ListScopedHString json;

    if (!className.Create(
        RuntimeClass_Windows_Data_Json_JsonValue,
        wcslen(RuntimeClass_Windows_Data_Json_JsonValue)))
    {
        return items;
    }

    ListComPtr<ABI::Windows::Data::Json::IJsonValueStatics> statics;

    HRESULT hr = RoGetActivationFactory(
        className.Get(),
        IID_JSON_VALUE_STATICS,
        reinterpret_cast<void**>(statics.ReleaseAndGetAddressOf()));

    if (FAILED(hr))
        return items;

    if (!json.Create(text.c_str(), text.size()))
        return items;

    ListComPtr<ABI::Windows::Data::Json::IJsonValue> value;

    if (FAILED(statics->Parse(json.Get(), value.ReleaseAndGetAddressOf())))
        return items;

    ListComPtr<ABI::Windows::Data::Json::IJsonArray> array;

    if (FAILED(value->GetArray(&array)))
        return items;

    ParseItemArray(array.Get(), items);

    return items;
}

// ========================================
// JSON serialization helpers
// ========================================

static void AppendJsonEscaped(std::wstring& out, const std::wstring& s);
static void SerializeItem(const Item& item, std::wstring& out);
static void SerializeItemArray(const std::vector<Item>& items, std::wstring& out);

static void AppendJsonEscaped(std::wstring& out, const std::wstring& s)
{
    out += L"\"";

    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\n': out += L"\\n";  break;
        case L'\r': out += L"\\r";  break;
        case L'\t': out += L"\\t";  break;
        default:    out += c;       break;
        }
    }

    out += L"\"";
}

static void SerializeItem(const Item& item, std::wstring& out)
{
    out += L"{\"name\":";
    AppendJsonEscaped(out, item.name);

    out += L",\"path\":";
    AppendJsonEscaped(out, item.path);

    out += L",\"children\":";
    SerializeItemArray(item.children, out);

    out += L"}";
}

static void SerializeItemArray(const std::vector<Item>& items, std::wstring& out)
{
    out += L"[";

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0)
            out += L",";

        SerializeItem(items[i], out);
    }

    out += L"]";
}

// ========================================
// Public API
// ========================================

bool UpdateList(const std::vector<Item>& items)
{
    std::wstring json;
    SerializeItemArray(items, json);
    json += L"\r\n";

    return WriteTextFile(GetListPath(), json.c_str());
}

std::vector<Item> InitList()
{
    RoInitialize(RO_INIT_MULTITHREADED);

    const std::wstring path = GetListPath();

    // Create with default empty array when missing
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        WriteTextFile(path, defaultListString);
    }

    std::wstring text;

    if (ReadTextFile(path, text))
    {
        return ParseListJson(text);
    }

    return {};
}
