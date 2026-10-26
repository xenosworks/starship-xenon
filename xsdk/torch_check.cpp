// torch_check: xsdk milestone 64's first step (xsdk:docs/scope/64-starship-rom-extraction.md). Torch run on the console
// as the PC build's GameExtractor runs it (src/port/extractor/GameExtractor.cpp), on baserom.z64 in its folder with
// Starship's config.yml and assets/yaml beside it, writing sf64.o2r there; xsdk/compare_o2r.py on the PC compares it
// with the PC's, entry by entry.
#include <malloc.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <vector>

#include "Companion.h"
#include "xsdk/log.h"

int main() {
    std::ifstream file("baserom.z64", std::ios::binary);
    if (!file) {
        xsdk_log_write("torch_check: no baserom.z64 beside the .xex");
        return 1;
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    xsdk_log_write("torch_check: baserom.z64, %u bytes", (unsigned) rom.size());

    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&start]() {
        return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                 start)
            .count();
    };
    // Strings, as GameExtractor passes: literals would pick the constructor whose fourth parameter is the modding flag.
    const std::string here;
    Companion* const companion = new Companion(rom, ArchiveType::O2R, false, here, here);
    Companion::Instance = companion;
    // Each asset file as it starts, to see where the time goes.
    companion->gProgress = [&elapsed](const std::string& yamlPath, size_t done, size_t total) {
        xsdk_log_write("torch_check: %lld ms, file %u of %u %s", elapsed(), (unsigned) done, (unsigned) total,
                       yamlPath.c_str());
    };
    try {
        companion->Init(ExportType::Binary);
    } catch (const std::exception& e) {
        xsdk_log_write("torch_check: Torch failed: %s", e.what());
        return 1;
    }
    const long long ms = elapsed();
    const unsigned used_kb = (unsigned) (mallinfo().uordblks / 1024);
    // Init leaves the companion, as GameExtractor does; a game that goes on after extracting gives its memory back.
    delete companion;
    xsdk_log_write(
        "torch_check: sf64.o2r made in %lld ms; the heap holds %u KB, and %u KB once the companion's deleted", ms,
        used_kb, (unsigned) (mallinfo().uordblks / 1024));
    return 0;
}
