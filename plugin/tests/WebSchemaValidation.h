#pragma once
#include <map>
#include <regex>
#include <set>
#include <sstream>
struct Schemas {
    std::map<std::string, juce::var> files;
    bool load(const juce::File& dir) {
        for (auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.schema.json")) {
            juce::var v;
            if (juce::JSON::parse(f.loadFileAsString(), v).failed()) return false;
            files[f.getFileName().toStdString()] = v;
        }
        return !files.empty();
    }
    juce::var resolve(const std::string& ref, const std::string& curFile, std::string& fileOut) const {
        std::string file = curFile, ptr;
        const auto hash = ref.find('#');
        const std::string url = hash == std::string::npos ? ref : ref.substr(0, hash);
        if (hash != std::string::npos) ptr = ref.substr(hash + 1);
        if (!url.empty()) file = url.substr(url.find_last_of('/') + 1);
        fileOut = file;
        auto it = files.find(file);
        if (it == files.end()) return {};
        juce::var cur = it->second;
        std::stringstream ss(ptr);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) continue;
            cur = cur.getProperty(juce::String(seg), {});
        }
        return cur;
    }
    static bool isObj(const juce::var& v) { return v.getDynamicObject() != nullptr; }
    static bool typeOk(const juce::String& t, const juce::var& v) {
        if (t == "object") return isObj(v);
        if (t == "array") return v.isArray();
        if (t == "string") return v.isString();
        if (t == "boolean") return v.isBool();
        if (t == "null") return v.isVoid();
        if (t == "number") return v.isInt() || v.isInt64() || v.isDouble();
        if (t == "integer") return v.isInt() || v.isInt64() || (v.isDouble() && std::floor((double)v) == (double)v);
        return true;
    }
    bool validate(const juce::var& sch, const juce::var& v, const std::string& file, std::string& err, const std::string& path) const {
        if (sch.isBool()) { if (!(bool)sch) err = path + ": schema false"; return (bool)sch; }
        auto* o = sch.getDynamicObject();
        if (!o) return true;
        auto fail = [&](const std::string& m) { if (err.empty()) err = path + ": " + m; return false; };
        if (o->hasProperty("$ref")) {
            std::string f;
            const juce::var t = resolve(o->getProperty("$ref").toString().toStdString(), file, f);
            if (t.isVoid()) return fail("unresolved $ref " + o->getProperty("$ref").toString().toStdString());
            if (!validate(t, v, f, err, path)) return false;
        }
        if (o->hasProperty("type")) {
            const juce::var t = o->getProperty("type");
            bool any = false;
            if (t.isArray()) { for (auto& e : *t.getArray()) any |= typeOk(e.toString(), v); } else any = typeOk(t.toString(), v);
            if (!any) return fail("wrong type, expected " + juce::JSON::toString(t, true).toStdString());
        }
        if (o->hasProperty("const") && !(o->getProperty("const") == v)) return fail("const mismatch");
        if (o->hasProperty("enum")) {
            bool any = false;
            for (auto& e : *o->getProperty("enum").getArray()) any |= (e == v);
            if (!any) return fail("not in enum");
        }
        const bool num = v.isInt() || v.isInt64() || v.isDouble();
        if (num) {
            const double d = (double)v;
            if (o->hasProperty("minimum") && d < (double)o->getProperty("minimum")) return fail("below minimum");
            if (o->hasProperty("maximum") && d > (double)o->getProperty("maximum")) return fail("above maximum");
            if (o->hasProperty("exclusiveMinimum") && d <= (double)o->getProperty("exclusiveMinimum")) return fail("not above exclusiveMinimum");
        }
        if (v.isString()) {
            const int len = v.toString().length();
            if (o->hasProperty("minLength") && len < (int)o->getProperty("minLength")) return fail("string too short");
            if (o->hasProperty("maxLength") && len > (int)o->getProperty("maxLength")) return fail("string too long");
            if (o->hasProperty("pattern") && !std::regex_search(v.toString().toStdString(), std::regex(o->getProperty("pattern").toString().toStdString()))) return fail("pattern mismatch");
        }
        if (v.isArray()) {
            const int n = v.getArray()->size();
            if (o->hasProperty("minItems") && n < (int)o->getProperty("minItems")) return fail("too few items");
            if (o->hasProperty("maxItems") && n > (int)o->getProperty("maxItems")) return fail("too many items");
            if (o->hasProperty("items"))
                for (int i = 0; i < n; ++i) if (!validate(o->getProperty("items"), (*v.getArray())[i], file, err, path + "[" + std::to_string(i) + "]")) return false;
        }
        if (isObj(v)) {
            auto* io = v.getDynamicObject();
            if (o->hasProperty("required"))
                for (auto& r : *o->getProperty("required").getArray()) if (!io->hasProperty(juce::Identifier(r.toString()))) return fail("missing required " + r.toString().toStdString());
            const juce::var props = o->getProperty("properties");
            const int propertiesCount = io->getProperties().size();
            if (o->hasProperty("minProperties") && propertiesCount < (int)o->getProperty("minProperties")) return fail("too few properties");
            if (o->hasProperty("maxProperties") && propertiesCount > (int)o->getProperty("maxProperties")) return fail("too many properties");
            for (auto& nv : io->getProperties()) {
                const juce::var sub = isObj(props) ? props.getProperty(nv.name.toString(), juce::var()) : juce::var();
                bool known = isObj(props) && props.getDynamicObject()->hasProperty(nv.name);
                const auto patterns = o->getProperty("patternProperties");
                if (patterns.isObject()) for (const auto& pattern : patterns.getDynamicObject()->getProperties()) {
                    if (std::regex_search(nv.name.toString().toStdString(), std::regex(pattern.name.toString().toStdString()))) {
                        if (!validate(pattern.value, nv.value, file, err, path + "/" + nv.name.toString().toStdString())) return false;
                        known = true;
                    }
                }
                if (isObj(props) && props.getDynamicObject()->hasProperty(nv.name)) { if (!validate(sub, nv.value, file, err, path + "/" + nv.name.toString().toStdString())) return false; }
                else if (!known && o->hasProperty("additionalProperties")) {
                    const juce::var ap = o->getProperty("additionalProperties");
                    if (ap.isBool() && !(bool)ap) return fail("unexpected property " + nv.name.toString().toStdString());
                    if (isObj(ap) && !validate(ap, nv.value, file, err, path + "/" + nv.name.toString().toStdString())) return false;
                }
            }
        }
        if (o->hasProperty("allOf"))
            for (auto& s : *o->getProperty("allOf").getArray()) if (!validate(s, v, file, err, path)) return false;
        if (o->hasProperty("anyOf")) {
            bool any = false;
            for (auto& s : *o->getProperty("anyOf").getArray()) { std::string e2; any |= validate(s, v, file, e2, path); }
            if (!any) return fail("matches no anyOf branch");
        }
        if (o->hasProperty("oneOf")) {
            int n = 0;
            for (auto& s : *o->getProperty("oneOf").getArray()) { std::string e2; n += validate(s, v, file, e2, path) ? 1 : 0; }
            if (n != 1) return fail("matches " + std::to_string(n) + " oneOf branches");
        }
        if (o->hasProperty("if")) {
            std::string e2;
            if (validate(o->getProperty("if"), v, file, e2, path)) { if (o->hasProperty("then") && !validate(o->getProperty("then"), v, file, err, path)) return false; }
            else if (o->hasProperty("else") && !validate(o->getProperty("else"), v, file, err, path)) return false;
        }
        return true;
    }
};
