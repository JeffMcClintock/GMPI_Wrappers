/* se_gmpi_state_analysis - what a SynthEdit export's saved chunk contains, as MEASURED lines.
 *
 * Shared by the VST3 probe (se_gmpi_state_probe.cpp) and the CLAP probe
 * (se_gmpi_clap_state_probe.cpp), so the two wrappers are quoted by one reader. Header only,
 * no XML library: a probe scans attributes.
 *
 *   MEASURED chunk bytes            how big the saved chunk is
 *   MEASURED chunk params           every <Param id val> the chunk carries
 *   MEASURED chunk blob             a Param whose val is base64 of a <Preset> (the GMPI
 *                                   store's engine-preset blob), and every Param inside it
 *   MEASURED engine param <handle>  SAVED (by handle | in blob | as DAW id N) <val> | LOST,
 *                                   one per module-owned parameter in parameters.se.xml.
 *                                   "as DAW id" means the parameter's Index is a GMPI
 *                                   parameter the chunk carries directly.
 *   MEASURED survivors              N of M engine parameters saved
 *   MEASURED round-trip             for each restored Param, whether the saved chunk carries
 *                                   the same id and value; a blob is compared by the Params
 *                                   inside it (the controller re-mints it on save)
 */
#pragma once

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace se_state_analysis
{

inline std::string attr(const std::string& element, const char* name)
{
    const std::string key = std::string(" ") + name + "=\"";
    const size_t a = element.find(key);
    if (a == std::string::npos) return {};
    const size_t v = a + key.size();
    const size_t e = element.find('"', v);
    if (e == std::string::npos) return {};
    return element.substr(v, e - v);
}

/* every "<Tag ...>" or "<Tag .../>" opening element with that tag name */
inline std::vector<std::string> elements(const std::string& xml, const char* tag)
{
    std::vector<std::string> out;
    const std::string open = std::string("<") + tag;
    size_t p = 0;
    for (;;)
    {
        p = xml.find(open, p);
        if (p == std::string::npos) break;
        const char after = p + open.size() < xml.size() ? xml[p + open.size()] : '\0';
        if (after != ' ' && after != '/' && after != '>') { p += open.size(); continue; }
        const size_t e = xml.find('>', p);
        if (e == std::string::npos) break;
        out.push_back(xml.substr(p, e - p + 1));
        p = e + 1;
    }
    return out;
}

/* base64 as GMPI/Core/base64.h writes it (standard alphabet, '=' padding) */
inline bool base64Decode(const std::string& in, std::string& out)
{
    if (in.empty() || in.size() % 4 != 0) return false;
    auto val = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < in.size(); i += 4)
    {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = in[i + k];
            if (c == '=') { v[k] = 0; ++pad; }
            else { v[k] = val(c); if (v[k] < 0 || pad) return false; }
        }
        const unsigned triple = ((unsigned)v[0] << 18) | ((unsigned)v[1] << 12) | ((unsigned)v[2] << 6) | (unsigned)v[3];
        out.push_back((char)((triple >> 16) & 0xff));
        if (pad < 2) out.push_back((char)((triple >> 8) & 0xff));
        if (pad < 1) out.push_back((char)(triple & 0xff));
    }
    return true;
}

/* a Param value that is a base64-encoded <Preset> document: the engine-preset blob */
inline bool decodeBlobPreset(const std::string& val, std::string& xml)
{
    return base64Decode(val, xml) && xml.find("<Preset") != std::string::npos;
}

inline bool slurp(const std::string& path, std::string& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

struct EngineParam
{
    std::string handle, name, module, valueType, hostControl, index;
    bool isPrivate = false, stateful = true;
};

inline std::vector<EngineParam> readParametersSeXml(const std::string& xml)
{
    std::vector<EngineParam> out;
    for (const auto& el : elements(xml, "Parameter"))
    {
        EngineParam p;
        p.handle = attr(el, "Handle");
        if (p.handle.empty()) continue;
        p.name = attr(el, "Name");
        p.module = attr(el, "Module");
        p.valueType = attr(el, "ValueType");
        p.hostControl = attr(el, "HostControl");
        p.index = attr(el, "Index");
        p.isPrivate = attr(el, "Private") == "1";
        p.stateful = attr(el, "persistant") != "0";
        out.push_back(p);
    }
    return out;
}

/* The report. 'chunk' is the saved <Preset> text (envelope already stripped), 'resources' the
 * bundle's Resources folder (for parameters.se.xml), 'restoreXml' the chunk that was restored
 * first, or empty. */
inline void report(const std::string& chunk, const std::string& resources, const std::string& restoreXml)
{
    printf("\nMEASURED chunk bytes            %zu\n", chunk.size());

    std::map<std::string, std::string> chunkParams; /* outer: id -> val (SE handles on se_vst3, GMPI ids on the GMPI wrappers) */
    std::map<std::string, std::string> blobParams;  /* inner: SE handle -> val, from any engine-preset blob the chunk carries */
    for (const auto& el : elements(chunk, "Param"))
    {
        const auto id = attr(el, "id");
        const auto val = attr(el, "val");
        const auto midi = attr(el, "MIDI");
        chunkParams[id] = val;
        std::string inner;
        if (decodeBlobPreset(val, inner))
        {
            printf("MEASURED chunk blob             id=%s carries a %zu byte <Preset>\n", id.c_str(), inner.size());
            for (const auto& iel : elements(inner, "Param"))
            {
                const auto iid = attr(iel, "id");
                const auto ival = attr(iel, "val");
                const auto imidi = attr(iel, "MIDI");
                blobParams[iid] = ival;
                printf("MEASURED chunk blob               id=%s val=\"%s\"%s%s\n", iid.c_str(), ival.c_str(), imidi.empty() ? "" : " MIDI=", imidi.c_str());
            }
        }
        else
            printf("MEASURED chunk params           id=%s val=\"%s\"%s%s\n", id.c_str(), val.c_str(), midi.empty() ? "" : " MIDI=", midi.c_str());
    }
    if (chunkParams.empty()) printf("MEASURED chunk params           (none)\n");

    std::string paramsXml;
    const std::string paramsPath = resources + "/parameters.se.xml";
    if (!slurp(paramsPath, paramsXml))
        printf("MEASURED engine params          parameters.se.xml not found at %s (use --resources)\n", paramsPath.c_str());
    else
    {
        int engineTotal = 0, engineSaved = 0;
        for (const auto& p : readParametersSeXml(paramsXml))
        {
            if (p.module.empty()) continue; /* host controls (program name etc.) are not the engine's patch */
            if (!p.stateful) continue;
            ++engineTotal;
            /* three ways a parameter can survive: by handle in the chunk (se_vst3), by handle
             * inside the engine-preset blob, or as the GMPI/DAW parameter its Index names */
            std::string how, val;
            if (auto it = chunkParams.find(p.handle); it != chunkParams.end()) { how = "by handle"; val = it->second; }
            else if (auto bt = blobParams.find(p.handle); bt != blobParams.end()) { how = "in blob"; val = bt->second; }
            else if (!p.index.empty()) { if (auto dt = chunkParams.find(p.index); dt != chunkParams.end()) { how = "as DAW id " + p.index; val = dt->second; } }
            const bool saved = !how.empty();
            if (saved) ++engineSaved;
            printf("MEASURED engine param %-11s %s %-12s %-30s type=%s%s%s\n",
                   p.handle.c_str(), saved ? "SAVED" : "LOST ", saved ? how.c_str() : "", p.name.c_str(),
                   p.valueType.empty() ? "float" : p.valueType.c_str(),
                   p.isPrivate ? " private" : "",
                   saved ? (std::string(" val=\"") + val + "\"").c_str() : "");
        }
        printf("MEASURED survivors              %d of %d engine parameters saved\n", engineSaved, engineTotal);
    }

    if (!restoreXml.empty())
    {
        int asked = 0, roundTripped = 0;
        for (const auto& el : elements(restoreXml, "Param"))
        {
            const auto id = attr(el, "id");
            const auto val = attr(el, "val");
            std::string inner;
            if (decodeBlobPreset(val, inner))
            {
                /* a blob is re-minted by the controller on save, so compare what is INSIDE it */
                for (const auto& iel : elements(inner, "Param"))
                {
                    const auto iid = attr(iel, "id");
                    const auto ival = attr(iel, "val");
                    ++asked;
                    const auto bt = blobParams.find(iid);
                    const bool same = bt != blobParams.end() && bt->second == ival;
                    if (same) ++roundTripped;
                    printf("MEASURED round-trip blob id=%-11s %s  restored \"%s\" saved \"%s\"\n", iid.c_str(), same ? "SAME" : "DIFF",
                           ival.c_str(), bt == blobParams.end() ? "(absent)" : bt->second.c_str());
                }
                continue;
            }
            ++asked;
            const auto it = chunkParams.find(id);
            const bool same = it != chunkParams.end() && it->second == val;
            if (same) ++roundTripped;
            printf("MEASURED round-trip id=%-11s %s  restored \"%s\" saved \"%s\"\n", id.c_str(), same ? "SAME" : "DIFF",
                   val.c_str(), it == chunkParams.end() ? "(absent)" : it->second.c_str());
        }
        printf("MEASURED round-trip             %d of %d restored params came back unchanged\n", roundTripped, asked);
    }
}

/* The output's fundamental from rising zero crossings: with a fixture whose pitch is not a DAW
 * parameter, that frequency is the one number that proves a restored preset reached the ENGINE. */
struct FrequencyEstimate
{
    int risingCrossings = 0;
    long long samples = 0;
    float lastSample = 0.0f;

    void feed(const float* ch0, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            if (lastSample < 0.0f && ch0[i] >= 0.0f) ++risingCrossings;
            lastSample = ch0[i];
            ++samples;
        }
    }
    void print(double sampleRate) const
    {
        if (samples > 0)
            printf("MEASURED audio Hz               %.1f (%d rising zero crossings in %lld samples at %.0f Hz)\n",
                   risingCrossings * sampleRate / (double)samples, risingCrossings, samples, sampleRate);
    }
};

} // namespace se_state_analysis
