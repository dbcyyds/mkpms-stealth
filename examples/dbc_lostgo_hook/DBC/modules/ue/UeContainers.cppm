module;

#include <codecvt>
#include <cstdint>
#include <locale>
#include <string>

export module UeContainers;

export namespace UC {

template<typename T>
class TArray {
public:
    T* Data{};
    int32_t Count{};
    int32_t Max{};

    T& operator[](int32_t i) { return Data[i]; }
    const T& operator[](int32_t i) const { return Data[i]; }
    int32_t Num() const { return Count; }
};

struct FString : private TArray<unsigned short> {
    FString() = default;

    FString(const std::string& s) {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        const std::wstring ws = conv.from_bytes(s);
        Max = Count = ws.empty() ? 0 : static_cast<int32_t>(ws.length() + 1);
        if (Count) {
            Data = new unsigned short[Count];
            for (int32_t i = 0; i < Count; ++i)
                Data[i] = static_cast<unsigned short>(ws[i]);
        }
    }

    int Length() const { return Count; }

    std::string ToUtf8() const {
        if (Count <= 0 || !Data)
            return {};
        std::wstring ws;
        ws.reserve(static_cast<size_t>(Count));
        for (int32_t i = 0; i < Count; ++i) {
            const unsigned short ch = Data[i];
            if (ch == 0)
                break;
            ws.push_back(static_cast<wchar_t>(ch));
        }
        if (ws.empty())
            return {};
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        return conv.to_bytes(ws);
    }
};

}