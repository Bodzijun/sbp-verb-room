#include "directivity.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
extern char **environ;
#endif

// No command interpreter. Each argument retains its literal value.
int runPython(const std::vector<std::string>& arguments) {
    const char* configured=std::getenv("VERBDIR_PYTHON");
#ifdef _WIN32
    const std::string executable=configured?configured:"python";
    auto quote=[](const std::wstring& value) {
        std::wstring out=L"\"";size_t slashes=0;
        for(wchar_t ch:value) {
            if(ch==L'\\') {++slashes;continue;}
            if(ch==L'"') {out.append(slashes*2+1,L'\\');out+=ch;}
            else {out.append(slashes,L'\\');out+=ch;}
            slashes=0;
        }
        out.append(slashes*2,L'\\');out+=L'"';return out;
    };
    std::wstring command=quote(fs::path(executable).wstring());
    for(const auto& arg:arguments) command+=L" "+quote(fs::path(arg).wstring());
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&process)) {
        std::cerr<<"Cannot launch Python (Windows error "<<GetLastError()<<"). Install Python or set VERBDIR_PYTHON to its executable.\n";return -1;
    }
    WaitForSingleObject(process.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return (int)code;
#else
    std::string executable=configured?configured:"python3";
    std::vector<std::string> storage{executable};storage.insert(storage.end(),arguments.begin(),arguments.end());
    std::vector<char*> args;for(auto& value:storage)args.push_back(&value[0]);args.push_back(nullptr);
    pid_t child;const int error=posix_spawnp(&child,executable.c_str(),nullptr,nullptr,args.data(),environ);
    if(error) {std::cerr<<"Cannot launch Python: "<<std::strerror(error)<<". Install Python 3 or set VERBDIR_PYTHON.\n";return -1;}
    int status=0;while(waitpid(child,&status,0)<0) {if(errno!=EINTR)return -1;}
    return WIFEXITED(status)?WEXITSTATUS(status):1;
#endif
}


namespace {

struct DirectionalSample {
    double ux = 1.0, uy = 0.0, uz = 0.0;
    double weight = 1.0;
    std::array<double, verb::kDirectivityBands> gain{};
};

void radix2Fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * verb::kDirectivityPi / (double)len;
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

std::array<double, verb::kDirectivityBands> octaveGainsFromIr(const std::vector<double>& ir, double sampleRate) {
    size_t nfft = 4096;
    while (nfft < ir.size()) nfft <<= 1;
    std::vector<std::complex<double>> buf(nfft, {0.0, 0.0});
    for (size_t i = 0; i < ir.size(); ++i) buf[i] = {ir[i], 0.0};
    radix2Fft(buf);

    constexpr double kFc[verb::kDirectivityBands] = {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};
    const double sqrt2 = std::sqrt(2.0);
    const size_t nBins = nfft / 2 + 1;

    std::array<double, verb::kDirectivityBands> out{};
    for (int b = 0; b < verb::kDirectivityBands; ++b) {
        const double flo = kFc[b] / sqrt2;
        const double fhi = kFc[b] * sqrt2;
        double sumSq = 0.0;
        int count = 0;
        for (size_t k = 1; k < nBins; ++k) {
            const double f = (double)k * sampleRate / (double)nfft;
            if (f >= flo && f <= fhi) {
                sumSq += std::norm(buf[k]);
                ++count;
            }
        }
        if (count == 0) {
            const size_t k = std::clamp((size_t)std::llround(kFc[b] * (double)nfft / sampleRate), (size_t)1, nBins - 1);
            sumSq = std::norm(buf[k]);
            count = 1;
        }
        out[(size_t)b] = std::sqrt(std::max(1e-18, sumSq / (double)count));
    }
    return out;
}

bool readWavMono(const fs::path& p, std::vector<double>& samples, double& sampleRate, std::string& err) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        err = "Cannot open WAV file: " + p.string();
        return false;
    }
    f.seekg(0,std::ios::end); const auto fileBytes=f.tellg(); f.seekg(0);
    if(fileBytes<12 || fileBytes>64*1024*1024){err="WAV must be 12 bytes to 64 MiB: "+p.string();return false;}
    char riff[4]{}, wave[4]{};
    uint32_t riffSize = 0;
    f.read(riff, 4);
    f.read(reinterpret_cast<char*>(&riffSize), 4);
    f.read(wave, 4);
    if (!f || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        err = "Not a valid RIFF/WAVE file: " + p.string();
        return false;
    }

    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0, blockAlign = 0;
    uint32_t sr = 0, byteRate = 0;
    std::vector<uint8_t> dataBytes;

    while (f) {
        char chunkId[4]{};
        uint32_t chunkSize = 0;
        if (!f.read(chunkId, 4) || !f.read(reinterpret_cast<char*>(&chunkSize), 4)) break;
        const auto offset=f.tellg();
        if(offset<0 || (uint64_t)chunkSize > (uint64_t)(fileBytes-offset)) {err="Truncated WAV chunk: "+p.string();return false;}
        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if(chunkSize<16){err="Short WAV fmt chunk";return false;}
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&numChannels), 2);
            f.read(reinterpret_cast<char*>(&sr), 4);
            f.read(reinterpret_cast<char*>(&byteRate), 4);
            f.read(reinterpret_cast<char*>(&blockAlign), 2);
            f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (chunkSize > 16) f.seekg(chunkSize - 16, std::ios::cur);
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataBytes.resize(chunkSize);
            f.read(reinterpret_cast<char*>(dataBytes.data()), chunkSize);
            if(!f){err="Truncated WAV samples";return false;}
            break;
        } else {
            f.seekg(chunkSize + (chunkSize & 1u), std::ios::cur);
        }
    }

    if (sr == 0 || numChannels == 0 || dataBytes.empty()) {
        err = "Incomplete WAV header or empty data in: " + p.string();
        return false;
    }
    sampleRate = (double)sr;
    const size_t bytesPerSample = bitsPerSample / 8;
    const size_t frameSize = bytesPerSample * numChannels;
    if (frameSize == 0) {
        err = "Unsupported bitsPerSample in: " + p.string();
        return false;
    }
    if(blockAlign!=frameSize || dataBytes.size()%frameSize!=0){err="Invalid WAV frame alignment";return false;}
    const size_t nFrames = dataBytes.size() / frameSize;
    samples.resize(nFrames, 0.0);

    for (size_t i = 0; i < nFrames; ++i) {
        const uint8_t* ptr = dataBytes.data() + i * frameSize;
        double v = 0.0;
        if (audioFormat == 3 && bitsPerSample == 32) {
            float fl = 0.0f;
            std::memcpy(&fl, ptr, 4);
            v = (double)fl;
        } else if (audioFormat == 3 && bitsPerSample == 64) {
            std::memcpy(&v, ptr, 8);
        } else if (audioFormat == 1 && bitsPerSample == 16) {
            int16_t s = 0;
            std::memcpy(&s, ptr, 2);
            v = (double)s / 32768.0;
        } else if (audioFormat == 1 && bitsPerSample == 24) {
            int32_t s = (int32_t)ptr[0] | ((int32_t)ptr[1] << 8) | ((int32_t)(int8_t)ptr[2] * 65536);
            v = (double)s / 8388608.0;
        } else if (audioFormat == 1 && bitsPerSample == 32) {
            int32_t s = 0;
            std::memcpy(&s, ptr, 4);
            v = (double)s / 2147483648.0;
        } else {
            err = "Unsupported WAV sample format in: " + p.string();
            return false;
        }
        samples[i] = v;
    }
    return true;
}

bool solveLinearSystem(int n, std::vector<double>& A, std::vector<double>& b) {
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double maxAbs = std::fabs(A[(size_t)(col * n + col)]);
        for (int row = col + 1; row < n; ++row) {
            const double v = std::fabs(A[(size_t)(row * n + col)]);
            if (v > maxAbs) {
                maxAbs = v;
                pivot = row;
            }
        }
        if (maxAbs < 1e-14) return false;
        if (pivot != col) {
            for (int j = col; j < n; ++j)
                std::swap(A[(size_t)(col * n + j)], A[(size_t)(pivot * n + j)]);
            std::swap(b[(size_t)col], b[(size_t)pivot]);
        }
        const double diag = A[(size_t)(col * n + col)];
        for (int j = col; j < n; ++j) A[(size_t)(col * n + j)] /= diag;
        b[(size_t)col] /= diag;

        for (int row = 0; row < n; ++row) {
            if (row == col) continue;
            const double factor = A[(size_t)(row * n + col)];
            if (std::fabs(factor) < 1e-18) continue;
            for (int j = col; j < n; ++j)
                A[(size_t)(row * n + j)] -= factor * A[(size_t)(col * n + j)];
            b[(size_t)row] -= factor * b[(size_t)col];
        }
    }
    return true;
}

bool fitSamplesToProfile(
    const std::vector<DirectionalSample>& samples,
    int order,
    double regLambda,
    verb::DirectivityProfile& p,
    std::string& err)
{
    order = std::clamp(order, 0, verb::kMaxDirectivityOrder);
    p.order = order;
    const int nCoeffs = verb::directivityNumCoeffs(order);
    if ((int)samples.size() < nCoeffs) {
        err = "Need at least " + std::to_string(nCoeffs) + " directional samples for order " +
              std::to_string(order) + ", got " + std::to_string(samples.size()) + ".";
        return false;
    }

    // Build normal equations (Y^T W Y + lambda * L) c = Y^T W g
    std::vector<double> XtWX((size_t)(nCoeffs * nCoeffs), 0.0);
    std::array<std::vector<double>, verb::kDirectivityBands> XtWy;
    for (int b = 0; b < verb::kDirectivityBands; ++b)
        XtWy[(size_t)b].assign((size_t)nCoeffs, 0.0);

    double wSum = 0.0;
    for (const auto& s : samples) wSum += s.weight;
    const double wScale = (wSum > 1e-12) ? (1.0 / wSum) : 1.0;

    double y[verb::kMaxDirectivityCoeffs];
    for (const auto& s : samples) {
        const double w = s.weight * wScale;
        verb::evalRealSH(order, s.ux, s.uy, s.uz, y);
        for (int i = 0; i < nCoeffs; ++i) {
            const double wyi = w * y[i];
            for (int j = i; j < nCoeffs; ++j)
                XtWX[(size_t)(i * nCoeffs + j)] += wyi * y[j];
            for (int b = 0; b < verb::kDirectivityBands; ++b)
                XtWy[(size_t)b][(size_t)i] += wyi * s.gain[(size_t)b];
        }
    }
    for (int i = 0; i < nCoeffs; ++i)
        for (int j = 0; j < i; ++j)
            XtWX[(size_t)(i * nCoeffs + j)] = XtWX[(size_t)(j * nCoeffs + i)];

    // Tikhonov Laplace-Beltrami regularization l*(l+1)
    for (int l = 0; l <= order; ++l) {
        const double pen = regLambda * (1.0 + 0.25 * (double)(l * (l + 1)));
        for (int m = -l; m <= l; ++m) {
            const int k = l * l + l + m;
            XtWX[(size_t)(k * nCoeffs + k)] += pen;
        }
    }

    const auto& qNodes = verb::sphericalQuadratureNodes();

    for (int b = 0; b < verb::kDirectivityBands; ++b) {
        std::vector<double> A = XtWX;
        std::vector<double> rhs = XtWy[(size_t)b];
        if (!solveLinearSystem(nCoeffs, A, rhs)) {
            err = "Singular SH least-squares matrix on band " + std::to_string(b) + ".";
            return false;
        }

        // Check for negative lobes on dense quadrature grid; if present, re-project clamped non-negative gains
        double minVal = 1e9;
        for (const auto& q : qNodes) {
            verb::evalRealSH(order, q.ux, q.uy, q.uz, y);
            double sum = 0.0;
            for (int k = 0; k < nCoeffs; ++k) sum += rhs[(size_t)k] * y[k];
            minVal = std::min(minVal, sum);
        }
        if (minVal < 1e-2) {
            std::vector<double> cClamped((size_t)nCoeffs, 0.0);
            for (const auto& q : qNodes) {
                verb::evalRealSH(order, q.ux, q.uy, q.uz, y);
                double sum = 0.0;
                for (int k = 0; k < nCoeffs; ++k) sum += rhs[(size_t)k] * y[k];
                const double gPos = std::max(1e-2, sum);
                for (int k = 0; k < nCoeffs; ++k)
                    cClamped[(size_t)k] += q.weight * gPos * y[k];
            }
            rhs = std::move(cClamped);
        }

        p.coeffs[(size_t)b].resize((size_t)nCoeffs);
        for (int k = 0; k < nCoeffs; ++k)
            p.coeffs[(size_t)b][(size_t)k] = (float)rhs[(size_t)k];
    }

    return verb::normalizeProfilePower(p, &err);
}

bool loadCsvSamples(
    const fs::path& csvPath,
    bool valuesInDb,
    bool clockwiseAzimuth,
    std::vector<DirectionalSample>& out,
    std::string& err)
{
    std::ifstream in(csvPath);
    if (!in) {
        err = "Cannot open CSV file: " + csvPath.string();
        return false;
    }

    out.clear();
    std::string rawLine;
    int lineNo = 0;
    while (std::getline(in, rawLine)) {
        ++lineNo;
        std::string line = verb::detail::trimStr(rawLine);
        if (line.empty() || line[0] == '#') continue;

        for (char& c : line)
            if (c == ',' || c == ';') c = ' ';

        std::istringstream ls(line);
        double azDeg = 0.0, elDeg = 0.0;
        if (!(ls >> azDeg >> elDeg)) {
            if (lineNo == 1) continue;  // header row
            err = "CSV line " + std::to_string(lineNo) + ": invalid azimuth/elevation.";
            return false;
        }
        std::array<double, verb::kDirectivityBands> g{};
        for (int b = 0; b < verb::kDirectivityBands; ++b) {
            double v = 0.0;
            if (!(ls >> v) || !std::isfinite(v)) {
                err = "CSV line " + std::to_string(lineNo) + ": expected 7 finite band values after azimuth/elevation.";
                return false;
            }
            g[(size_t)b] = valuesInDb ? std::pow(10.0, v / 20.0) : std::max(1e-6, v);
        }

        const double azRad = (clockwiseAzimuth ? -azDeg : azDeg) * (verb::kDirectivityPi / 180.0);
        const double elRad = elDeg * (verb::kDirectivityPi / 180.0);
        const double ce = std::cos(elRad);
        DirectionalSample s;
        s.ux = ce * std::cos(azRad);
        s.uy = ce * std::sin(azRad);
        s.uz = std::sin(elRad);
        s.weight = std::max(0.05, ce);
        s.gain = g;
        out.push_back(s);
    }

    if (out.empty()) {
        err = "CSV file contains no data rows: " + csvPath.string();
        return false;
    }
    return true;
}

bool parseAzElFromFilename(const std::string& name, double& azDeg, double& elDeg) {
    // Matches e.g. Calibrated_IR_elevation_-10_azimuth_125.wav or az_0_el_15.wav
    std::string s = name;
    for (char& c : s) {
        if ((c >= '0' && c <= '9') || c == '-' || c == '.') continue;
        c = ' ';
    }
    const auto elPos = name.find("elevation");
    const auto azPos = name.find("azimuth");
    if (elPos != std::string::npos && azPos != std::string::npos) {
        auto readAfter = [&](size_t pos) -> double {
            while (pos < name.size() && !(std::isdigit((unsigned char)name[pos]) || name[pos] == '-')) ++pos;
            return std::strtod(name.c_str() + pos, nullptr);
        };
        elDeg = readAfter(elPos + 9);
        azDeg = readAfter(azPos + 7);
        return true;
    }
    return false;
}

bool loadWavDirSamples(
    const fs::path& dirPath,
    bool clockwiseAzimuth,
    std::vector<DirectionalSample>& out,
    std::string& err)
{
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        err = "WAV directory does not exist: " + dirPath.string();
        return false;
    }
    out.clear();
    for (const auto& entry : fs::recursive_directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".wav" && ext != ".WAV") continue;
        double azDeg = 0.0, elDeg = 0.0;
        if (!parseAzElFromFilename(entry.path().filename().string(), azDeg, elDeg)) continue;

        std::vector<double> ir;
        double sr = 48000.0;
        if (!readWavMono(entry.path(), ir, sr, err)) return false;

        const double azRad = (clockwiseAzimuth ? -azDeg : azDeg) * (verb::kDirectivityPi / 180.0);
        const double elRad = elDeg * (verb::kDirectivityPi / 180.0);
        const double ce = std::cos(elRad);
        DirectionalSample s;
        s.ux = ce * std::cos(azRad);
        s.uy = ce * std::sin(azRad);
        s.uz = std::sin(elRad);
        s.weight = std::max(0.05, ce);
        s.gain = octaveGainsFromIr(ir, sr);
        out.push_back(s);
    }
    if (out.empty()) {
        err = "No matching directional WAV files found in: " + dirPath.string();
        return false;
    }
    return true;
}

void printSummary(const verb::DirectivityProfile& p) {
    constexpr int kHz[verb::kDirectivityBands] = {125, 250, 500, 1000, 2000, 4000, 8000};
    std::cout << "Profile: " << p.id << " (" << p.name << "), order=" << p.order << "\n";
    std::cout << "Band (Hz) | MeanPwr | Front (dB) | Side90 (dB) | Rear180 (dB) | F/B (dB)\n";
    std::cout << "----------+---------+------------+-------------+--------------+---------\n";
    for (int b = 0; b < verb::kDirectivityBands; ++b) {
        const double pwr = verb::bandMeanPower(p, b);
        const double gf = p.evalBandLocal(b, 1.0, 0.0, 0.0);
        const double gs = p.evalBandLocal(b, 0.0, 1.0, 0.0);
        const double gb = p.evalBandLocal(b, -1.0, 0.0, 0.0);
        const double df = 20.0 * std::log10(std::max(1e-6, gf));
        const double ds = 20.0 * std::log10(std::max(1e-6, gs));
        const double db = 20.0 * std::log10(std::max(1e-6, gb));
        std::cout << std::setw(9) << kHz[b] << " | "
                  << std::fixed << std::setprecision(5) << pwr << " | "
                  << std::setw(10) << std::setprecision(2) << df << " | "
                  << std::setw(11) << ds << " | "
                  << std::setw(12) << db << " | "
                  << std::setw(7) << (df - db) << "\n";
    }
}

void printUsage() {
    std::cout << "Usage:\n"
              << "  verbdir_tool --csv <in.csv> -o <out.verbdir> --id <id> --name <name> [options]\n"
              << "  verbdir_tool --sofa <in.sofa_or_dir> -o <out.verbdir> --id <id> --name <name> [options]\n"
              << "  verbdir_tool --wav-dir <dir> -o <out.verbdir> --id <id> --name <name> [options]\n"
              << "  verbdir_tool --validate <in.verbdir>\n\n"
              << "SOFA: HDF5 only, requires Python 3 + numpy/h5py (no NetCDF3 Classic).\n"
              << "Assumed source frame: +x forward, +y left, +z up; positions already source-relative.\n"
              << "SourceView/SourceUp transforms are not applied. VERBDIR_PYTHON overrides the interpreter.\n"
              << "Options:\n"
              << "  --order <0..8>           Spherical harmonic order (default: 4)\n"
              << "  --category <str>         Category label (default: Custom)\n"
              << "  --license <str>          License string (default: Custom)\n"
              << "  --attribution <str>      Attribution string\n"
              << "  --model                  Mark profile as an analytic model\n"
              << "  --db                     CSV band columns are in dB instead of linear amplitude\n"
              << "  --clockwise-azimuth      Input azimuth increases clockwise (convert to CCW +y left)\n"
              << "  --condition <idx>        0-based condition/phoneme index for multi-condition SOFA\n"
              << "  --min-reliable-hz <hz>   Blend 125 Hz band toward omni when chamber cutoff > 125 Hz\n"
              << "  --reg <lambda>           SH Tikhonov regularization weight (default: 1e-4)\n";
}

}  // namespace

int convertMain(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    std::string csvIn, sofaIn, wavDirIn, validateIn, outPath;
    std::string id = "custom_profile";
    std::string name = "Custom Directivity";
    std::string category = "Custom";
    std::string license = "Custom";
    std::string attribution;
    bool isModel = false;
    bool valuesInDb = false;
    bool clockwiseAz = false;
    int conditionIdx = -1;
    double minReliableHz = 0.0;
    int order = verb::kDefaultDirectivityOrder;
    double regLambda = 1e-4;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needVal = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value after " << flag << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--csv") {
            csvIn = needVal("--csv");
        } else if (arg == "--sofa") {
            sofaIn = needVal("--sofa");
        } else if (arg == "--wav-dir") {
            wavDirIn = needVal("--wav-dir");
        } else if (arg == "--validate") {
            validateIn = needVal("--validate");
        } else if (arg == "-o" || arg == "--output") {
            outPath = needVal("-o");
        } else if (arg == "--id") {
            id = needVal("--id");
        } else if (arg == "--name") {
            name = needVal("--name");
        } else if (arg == "--category") {
            category = needVal("--category");
        } else if (arg == "--license") {
            license = needVal("--license");
        } else if (arg == "--attribution") {
            attribution = needVal("--attribution");
        } else if (arg == "--order") {
            order = std::stoi(needVal("--order"));
        } else if (arg == "--reg") {
            regLambda = std::stod(needVal("--reg"));
        } else if (arg == "--condition") {
            conditionIdx = std::stoi(needVal("--condition"));
        } else if (arg == "--min-reliable-hz") {
            minReliableHz = std::stod(needVal("--min-reliable-hz"));
        } else if (arg == "--model") {
            isModel = true;
        } else if (arg == "--db") {
            valuesInDb = true;
        } else if (arg == "--clockwise-azimuth") {
            clockwiseAz = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage();
            return 1;
        }
    }

    if (!validateIn.empty()) {
        std::ifstream f(validateIn, std::ios::binary);
        if (!f) {
            std::cerr << "Error: cannot open " << validateIn << "\n";
            return 1;
        }
        std::string bounded(verb::kMaxVerbdirBytes+1,'\0');
        f.read(&bounded[0],(std::streamsize)bounded.size());bounded.resize((size_t)f.gcount());
        if(bounded.size()>verb::kMaxVerbdirBytes){std::cerr<<"INVALID: .verbdir exceeds 64 KB.\n";return 1;}
        std::ostringstream ss; ss<<bounded;
        verb::DirectivityProfile p;
        std::string err;
        if (!verb::parseVerbdir(ss.str(), p, err)) {
            std::cerr << "INVALID: " << err << "\n";
            return 1;
        }
        printSummary(p);
        return 0;
    }

    if (outPath.empty()) {
        std::cerr << "Error: output path (-o <out.verbdir>) is required.\n";
        return 1;
    }

    std::vector<DirectionalSample> samples;
    std::string err;

    if (!sofaIn.empty()) {
        const fs::path tmpCsv = fs::path(outPath).replace_extension(".tmp_sofa.csv");
        fs::path helperPath = fs::path(argv[0]).parent_path() / ".." / "tools" / "sofa_to_verbdir.py";
        if (!fs::exists(helperPath)) helperPath = fs::path("tools") / "sofa_to_verbdir.py";
        if (!fs::exists(helperPath)) {
            std::cerr << "Error: cannot locate tools/sofa_to_verbdir.py for SOFA extraction.\n";
            return 1;
        }
        std::vector<std::string> args{helperPath.string(),sofaIn,"-o",tmpCsv.string()};
        if(clockwiseAz)args.push_back("--clockwise-azimuth");
        if(conditionIdx>=0){args.push_back("--condition");args.push_back(std::to_string(conditionIdx));}
        if(minReliableHz>0){args.push_back("--min-reliable-hz");args.push_back(std::to_string(minReliableHz));}
        const int rc=runPython(args);
        if(rc==3)std::cerr<<"Missing Python packages: install numpy and h5py with python -m pip install numpy h5py.\n";
        if (rc != 0 || !fs::exists(tmpCsv)) {
            std::cerr << "Error: SOFA extraction failed (exit code " << rc << ").\n";
            return 1;
        }
        const bool ok = loadCsvSamples(tmpCsv, false, false, samples, err);
        std::error_code ec;
        fs::remove(tmpCsv, ec);
        if (!ok) {
            std::cerr << "Error loading extracted SOFA samples: " << err << "\n";
            return 1;
        }
    } else if (!csvIn.empty()) {
        if (!loadCsvSamples(csvIn, valuesInDb, clockwiseAz, samples, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
    } else if (!wavDirIn.empty()) {
        if (!loadWavDirSamples(wavDirIn, clockwiseAz, samples, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
    } else {
        std::cerr << "Error: specify one of --csv, --sofa, --wav-dir, or --validate.\n";
        return 1;
    }

    verb::DirectivityProfile p;
    p.id = id;
    p.name = name;
    p.category = category;
    p.license = license;
    p.attribution = attribution;
    p.isModel = isModel;

    if (!fitSamplesToProfile(samples, order, regLambda, p, err)) {
        std::cerr << "Error fitting SH profile: " << err << "\n";
        return 1;
    }

    const std::string text = verb::writeVerbdir(p);
    verb::DirectivityProfile roundTrip;
    if (!verb::parseVerbdir(text, roundTrip, err)) {
        std::cerr << "Error validating generated .verbdir: " << err << "\n";
        return 1;
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "Error writing output file: " << outPath << "\n";
        return 1;
    }
    out << text;
    out.close();

    printSummary(roundTrip);
    return 0;
}

int main(int argc,char** argv) {
    try {return convertMain(argc,argv);}
    catch(const std::exception& e){std::cerr<<"Conversion failed: "<<e.what()<<"\n";return 1;}
}
