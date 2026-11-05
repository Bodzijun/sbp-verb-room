// verb <scene.txt> <out_prefix> [--dry in.wav] [--source name]
//  - without --dry: writes <out_prefix>_<source>_ir.wav (one channel per receiver) for every source
//  - with --dry:    convolves the first channel of in.wav with the IR of the chosen source -> <out_prefix>_wet.wav
#include <cstdio>
#include <cstring>

#include "render.hpp"

using namespace verb;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: verb <scene.txt> <out_prefix> [--dry in.wav] [--source name]\n");
        return 2;
    }
    try {
        std::string dryPath, srcName;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (!std::strcmp(argv[i], "--dry")) dryPath = argv[i + 1];
            else if (!std::strcmp(argv[i], "--source")) srcName = argv[i + 1];
            else throw std::runtime_error(std::string("unknown option ") + argv[i]);
        }
        Scene sc = loadScene(argv[1]);
        std::string prefix = argv[2];
        std::printf("room: %zu walls, height %.2f m, %zu source(s), %zu receiver(s)\n", sc.walls.size(), sc.height,
                    sc.sources.size(), sc.receivers.size());
        for (size_t si = 0; si < sc.sources.size(); ++si) {
            const auto& s = sc.sources[si];
            if (!srcName.empty() && s.name != srcName) continue;
            if (srcName.empty() && !dryPath.empty() && si > 0) break;
            Wav ir = renderIR(sc, (int)si, true);
            if (dryPath.empty()) {
                std::string p = prefix + "_" + s.name + "_ir.wav";
                writeWav(p, ir);
                std::printf("  wrote %s\n", p.c_str());
            } else {
                Wav dry = readWav(dryPath);
                if (dry.sr != ir.sr) throw std::runtime_error("sample rate mismatch: dry file is " + std::to_string(dry.sr) + " Hz, scene is " + std::to_string(ir.sr) + " Hz");
                Wav wet;
                wet.sr = ir.sr;
                for (auto& c : ir.ch) wet.ch.push_back(convolve(dry.ch[0], c));
                writeWav(prefix + "_wet.wav", wet);
                std::printf("  wrote %s_wet.wav\n", prefix.c_str());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
