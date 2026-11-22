#include "WebResources.h"

#include <cstring>

namespace verbweb {

juce::String mimeTypeFor(const juce::String& path) {
    const auto ext = path.fromLastOccurrenceOf(".", false, false).toLowerCase();
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf") return "font/ttf";
    if (ext == "otf") return "font/otf";
    if (ext == "wasm") return "application/wasm";
    if (ext == "txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

namespace {
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decoding to UTF-8; false for malformed escapes or NUL.
bool percentDecode(const juce::String& in, juce::String& out) {
    std::string bytes;
    const std::string src(in.toRawUTF8());
    for (std::size_t i = 0; i < src.size(); ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c == '%') {
            if (i + 2 >= src.size()) return false;
            const int h = hexValue(src[i + 1]), l = hexValue(src[i + 2]);
            if (h < 0 || l < 0) return false;
            c = (unsigned char)(h * 16 + l);
            i += 2;
        }
        if (c == 0) return false;
        bytes.push_back((char)c);
    }
    out = juce::String::fromUTF8(bytes.data(), (int)bytes.size());
    return true;
}
}  // namespace

juce::String normaliseRequestPath(const juce::String& url) {
    auto p = url.upToFirstOccurrenceOf("#", false, false).upToFirstOccurrenceOf("?", false, false);
    juce::String decoded;
    if (!percentDecode(p, decoded)) return {};
    p = decoded;
    if (p.contains("\\") || p.contains(":")) return {};
    while (p.startsWithChar('/')) p = p.substring(1);
    if (p.isEmpty()) return "index.html";
    juce::StringArray parts;
    parts.addTokens(p, "/", "");
    for (int i = 0; i < parts.size(); ++i) {
        const auto& seg = parts[i];
        if (seg.isEmpty() || seg == "." || seg == "..") return {};
    }
    return p;
}

ResourceTable::ResourceTable() : files(embeddedFileTable(count)) {}
ResourceTable::ResourceTable(const EmbeddedFile* f, std::size_t c) : files(f), count(c) {}

std::optional<FileView> ResourceTable::find(const juce::String& requestPath) const {
    const auto rel = normaliseRequestPath(requestPath);
    if (rel.isEmpty()) return std::nullopt;
    for (std::size_t i = 0; i < count; ++i) {
        if (files[i].path != nullptr && rel == juce::String::fromUTF8(files[i].path)) {
            FileView v;
            v.data = files[i].data;
            v.size = files[i].size;
            v.mime = mimeTypeFor(rel);
            return v;
        }
    }
    return std::nullopt;
}

juce::WebBrowserComponent::ResourceProvider makeResourceProvider(const ResourceTable& table) {
    const ResourceTable* t = &table;
    return [t](const juce::String& url) -> std::optional<juce::WebBrowserComponent::Resource> {
        juce::WebBrowserComponent::Resource r;
        if (const auto f = t->find(url)) {
            r.data.resize(f->size);
            if (f->size > 0) std::memcpy(r.data.data(), f->data, f->size);
            r.mimeType = f->mime;
        } else {
            static const char kBody[] = "404 Not Found";
            r.data.resize(sizeof kBody - 1);
            std::memcpy(r.data.data(), kBody, sizeof kBody - 1);
            r.mimeType = "text/plain; charset=utf-8";
        }
        return r;
    };
}

bool isNavigationAllowed(const juce::String& url) {
    if (url == "about:blank") return true;
    const auto& root = juce::WebBrowserComponent::getResourceProviderRoot();
    // "https://juce.backend.evil.example/" does not start with "https://juce.backend/" (note the trailing slash).
    return root.isNotEmpty() && url.startsWith(root);
}

}  // namespace verbweb
