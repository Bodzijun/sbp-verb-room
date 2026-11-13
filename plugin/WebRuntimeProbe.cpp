// Kept in its own translation unit: it only needs the Windows and WebView2 headers, no JUCE.
// Asks the (statically linked) WebView2 loader which Evergreen/installed runtime version is available.
// This creates nothing and starts no process.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <WebView2.h>

#include <string>

namespace verbweb {

// Returns true and the version string when a WebView2 runtime is installed; otherwise false and the HRESULT.
bool probeWebView2RuntimeVersion(std::wstring& version, long& hresult) {
    LPWSTR v = nullptr;
    const HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &v);
    hresult = (long)hr;
    if (FAILED(hr) || v == nullptr || *v == L'\0') {
        if (v != nullptr) CoTaskMemFree(v);
        return false;
    }
    version = v;
    CoTaskMemFree(v);
    return true;
}

}  // namespace verbweb
