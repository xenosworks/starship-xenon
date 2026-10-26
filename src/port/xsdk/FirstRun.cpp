// Starship's first run on the Xbox 360 (xsdk milestone 64, xsdk:docs/scope/64-starship-rom-extraction.md). With no
// sf64.o2r beside the .xex, it's made there from the player's own ROM, a .z64, .n64 or .v64 of any name, by Torch as
// the PC build makes it (src/port/extractor/GameExtractor.cpp), on a screen of its own. The ROM is deleted once
// sf64.o2r is in place and opens, and Starship starts again, so the game gets all the memory Torch used.
#include "FirstRun.h"

#include <pthread.h>
#include <sys/statvfs.h>
#include <zip.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Companion.h"
#include "spdlog/spdlog.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/text.h"
#include "xsdk/xam.h"

namespace fs = std::filesystem;

namespace {

// The ROMs Starship makes its assets from, by SHA-1, as config.yml takes them: the US releases, as the cartridge holds
// them and decompressed.
const char* const kRomHashes[] = {
    "d8b1088520f7c5f81433292a9258c1184afa1457", // US 1.0
    "63b69f0ef36306257481afc250f9bc304c7162b2", // US 1.0, decompressed
    "09f0d105f476b00efa5303a3ebc42e60a7753b7a", // US 1.1
    "f7475fb11e7e6830f82883412638e8390791ab87", // US 1.1, decompressed
};

// Torch writes sf64.o2r into this folder; it's copied beside the .xex as sf64.o2r.part, and renamed once it's whole, so
// a run cut short leaves no sf64.o2r. FATX doesn't rename into another folder (0xC0000022 on the console).
const char kPartFolder[] = "sf64.part";
const char kArchive[] = "sf64.o2r";
const char kArchivePart[] = "sf64.o2r.part";
// sf64.o2r is 14.5 MB; room for it twice over, and Torch's hash file.
constexpr unsigned long long kSpaceBytes = 32ull * 1024 * 1024;
// Torch's stack: it runs on a thread of its own while the screen draws.
constexpr size_t kTorchStackBytes = 2 * 1024 * 1024;
constexpr int kFrameTimeoutMs = 200;
constexpr int kDoneShowMs = 4000;

// The Homebrew App Store's colours.
constexpr uint32_t kBackground = 0xFF0D0D0E;
constexpr uint32_t kText = 0xFFEEEEEA;
constexpr uint32_t kGrey = 0xFF8A8A86;
constexpr int32_t kLeft = 128;

class Screen {
  public:
    bool Open() {
        uint32_t width;
        uint32_t height;
        mOpen = NT_SUCCESS(xsdk_gpu_open()) && NT_SUCCESS(xsdk_text_open()) &&
                NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, 2, 1, &width, &height));
        mWidth = width;
        mHeight = height;
        return mOpen;
    }

    // A title, the lines under it, and a bar filled to fraction, 0 to 1, if it's at least 0.
    void Draw(const std::string& title, const std::string& lines, float fraction = -1.0f) {
        uint32_t address;
        if (!mOpen || !NT_SUCCESS(xsdk_present_begin(&address, kFrameTimeoutMs))) {
            return;
        }
        xsdk_gpu_set_target(mWidth, mHeight);
        xsdk_text_draw(kLeft, 200, 3, kText, title.c_str());
        xsdk_text_draw(kLeft, 290, 1, kText, lines.c_str());
        if (fraction >= 0.0f) {
            // A bar of text, filled as far as the work is, in one draw: a second string in a darker colour after it
            // came out speckled on the console.
            constexpr int kCells = 50;
            const int filled = std::clamp((int) (fraction * kCells + 0.5f), 0, kCells);
            const std::string bar = std::string(filled, '#') + std::string(kCells - filled, '-');
            xsdk_text_draw(kLeft, 430, 1, kText, bar.c_str());
        }
        xsdk_gpu_resolve(address, mWidth, mHeight, kBackground);
        xsdk_present_end();
    }

    // Shows a problem until a button's pressed.
    void WaitForButton(const std::string& title, const std::string& lines) {
        const std::string shown = lines + "\n\nPress A to go back to the dashboard.";
        bool held = AnyButton();
        for (;;) {
            Draw(title, shown);
            const bool pressed = AnyButton();
            if (pressed && !held) {
                return;
            }
            held = pressed;
        }
    }

  private:
    static bool AnyButton() {
        for (uint32_t pad = 0; pad < XSDK_XAM_USERS; pad++) {
            INPUT_STATE input;
            if (XamInputGetState(pad, 0, &input) == ERROR_SUCCESS &&
                (input.gamepad.buttons & (INPUT_GAMEPAD_A | INPUT_GAMEPAD_B | INPUT_GAMEPAD_START))) {
                return true;
            }
        }
        return false;
    }

    bool mOpen = false;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
};

struct Rom {
    std::string name;
    std::vector<uint8_t> data;
    std::string hash;
};

// The file put in .z64 order, the cartridge's: a .v64 has each 16-bit word's bytes swapped, a .n64 each 32-bit
// word's reversed. False if its first word is none of the three.
bool ToZ64(std::vector<uint8_t>& data) {
    if (data.size() < 0x40 || data.size() % 4 != 0) {
        return false;
    }
    const uint32_t first = (uint32_t) data[0] << 24 | (uint32_t) data[1] << 16 | (uint32_t) data[2] << 8 | data[3];
    if (first == 0x80371240) {
        return true;
    }
    if (first == 0x37804012) {
        for (size_t i = 0; i < data.size(); i += 2) {
            std::swap(data[i], data[i + 1]);
        }
        return true;
    }
    if (first == 0x40123780) {
        for (size_t i = 0; i < data.size(); i += 4) {
            std::swap(data[i], data[i + 3]);
            std::swap(data[i + 1], data[i + 2]);
        }
        return true;
    }
    return false;
}

// The ROMs beside the .xex, by name, each in .z64 order with its SHA-1; those that aren't N64 ROMs are left out.
std::vector<Rom> FindRoms(std::vector<std::string>& others) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(".", error)) {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (entry.is_regular_file(error) && (extension == ".z64" || extension == ".n64" || extension == ".v64")) {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    std::vector<Rom> roms;
    for (const auto& name : names) {
        std::ifstream file(name, std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (!ToZ64(data)) {
            others.push_back(name + " isn't an N64 ROM");
            continue;
        }
        const std::string hash = Companion::CalculateHash(data);
        roms.push_back({ name, std::move(data), hash });
    }
    return roms;
}

bool Takes(const std::string& hash) {
    return std::any_of(std::begin(kRomHashes), std::end(kRomHashes),
                       [&hash](const char* known) { return hash == known; });
}

struct Extraction {
    std::vector<uint8_t> rom;
    std::atomic<size_t> done{ 0 };
    std::atomic<size_t> total{ 0 };
    std::atomic<bool> finished{ false };
    std::string error;
};

void* RunTorch(void* argument) {
    auto* extraction = static_cast<Extraction*>(argument);
    // Strings, as GameExtractor passes: literals would pick the constructor whose fourth parameter is the modding flag.
    const std::string source;
    const std::string destination = kPartFolder;
    auto* companion = new Companion(extraction->rom, ArchiveType::O2R, false, source, destination);
    Companion::Instance = companion;
    companion->gProgress = [extraction](const std::string&, size_t done, size_t total) {
        extraction->total = total;
        extraction->done = done;
    };
    try {
        companion->Init(ExportType::Binary);
    } catch (const std::exception& e) { extraction->error = e.what(); }
    // The companion isn't deleted: freeing what it made took about 25 s on the console, and Starship starts again or
    // exits once sf64.o2r is in place, which gives all of it back.
    Companion::Instance = nullptr;
    extraction->finished = true;
    return nullptr;
}

// Whether sf64.o2r opens as the game opens an archive (libultraship's O2rArchive.cpp), with entries and its version.
bool ArchiveOpens() {
    zip_t* archive = zip_open(kArchive, ZIP_RDONLY, nullptr);
    if (archive == nullptr) {
        return false;
    }
    const bool whole = zip_get_num_entries(archive, 0) > 0 && zip_name_locate(archive, "version", 0) >= 0;
    zip_discard(archive);
    return whole;
}

// What a run leaves before sf64.o2r is in place, this run's or one cut short.
void RemovePartials() {
    std::error_code error;
    fs::remove_all(kPartFolder, error);
    fs::remove(kArchivePart, error);
}

std::string Megabytes(unsigned long long bytes) {
    char text[32];
    snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024.0));
    return text;
}

} // namespace

void XsdkMakeAssets() {
    Screen screen;
    if (!screen.Open()) {
        xsdk_log_write("first run: the screen didn't open");
        exit(1);
    }
    screen.Draw("Starship", "Looking for your ROM beside starship.xex...");

    std::vector<std::string> others;
    std::vector<Rom> roms = FindRoms(others);
    auto rom = std::find_if(roms.begin(), roms.end(), [](const Rom& candidate) { return Takes(candidate.hash); });
    if (rom == roms.end()) {
        std::string lines = "Starship makes its game files from your own copy of the game: a US ROM, version 1.0\n"
                            "or 1.1, as a .z64, .n64 or .v64 file of any name. Copy it beside starship.xex, then\n"
                            "start Starship again.";
        if (!roms.empty() || !others.empty()) {
            lines += "\n";
            for (const auto& other : roms) {
                lines += "\n" + other.name + " isn't one: its SHA-1 is " + other.hash + ".";
                xsdk_log_write("first run: %s, SHA-1 %s, isn't a ROM Starship takes", other.name.c_str(),
                               other.hash.c_str());
            }
            for (const auto& other : others) {
                lines += "\n" + other + ".";
            }
        }
        xsdk_log_write("first run: no ROM Starship takes beside the .xex");
        screen.WaitForButton("Starship needs your ROM", lines);
        exit(1);
    }

    struct statvfs space;
    if (statvfs(".", &space) == 0 && (unsigned long long) space.f_bavail * space.f_frsize < kSpaceBytes) {
        const unsigned long long free_bytes = (unsigned long long) space.f_bavail * space.f_frsize;
        xsdk_log_write("first run: %llu bytes free, %llu needed", free_bytes, kSpaceBytes);
        screen.WaitForButton("Not enough space", "Making sf64.o2r needs " + Megabytes(kSpaceBytes) +
                                                     " free on the drive starship.xex is on, and it has " +
                                                     Megabytes(free_bytes) +
                                                     ".\nFree some space, then start "
                                                     "Starship again.");
        exit(1);
    }

    xsdk_log_write("first run: making sf64.o2r from %s, SHA-1 %s", rom->name.c_str(), rom->hash.c_str());
    RemovePartials();
    Extraction extraction;
    extraction.rom = std::move(rom->data);
    const auto level = spdlog::get_level();
    const auto start = std::chrono::steady_clock::now();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kTorchStackBytes);
    pthread_t torch;
    if (pthread_create(&torch, &attributes, RunTorch, &extraction) != 0) {
        screen.WaitForButton("Starship couldn't start Torch", "Its thread didn't start.");
        exit(1);
    }
    pthread_attr_destroy(&attributes);
    while (!extraction.finished) {
        const size_t total = extraction.total;
        const size_t done = extraction.done;
        const long long seconds =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        char lines[256];
        snprintf(lines, sizeof(lines), "From %s, once: this takes about a minute.\n\n%u of %u files, %lld s",
                 rom->name.c_str(), (unsigned) done, (unsigned) total, seconds);
        screen.Draw("Making sf64.o2r", lines, total ? (float) done / total : 0.0f);
    }
    pthread_join(torch, nullptr);
    spdlog::set_level(level);

    std::error_code error;
    if (extraction.error.empty()) {
        fs::copy_file(fs::path(kPartFolder) / kArchive, kArchivePart, fs::copy_options::overwrite_existing, error);
    }
    if (extraction.error.empty() && !error) {
        fs::rename(kArchivePart, kArchive, error);
    }
    if (!extraction.error.empty() || error || !ArchiveOpens()) {
        const std::string why = !extraction.error.empty() ? "Torch stopped: " + extraction.error
                                : error                   ? "it couldn't be put in place: " + error.message()
                                                          : "it didn't open as an archive";
        xsdk_log_write("first run: sf64.o2r wasn't made: %s", why.c_str());
        fs::remove(kArchive, error);
        RemovePartials();
        screen.WaitForButton("sf64.o2r wasn't made",
                             "sf64.o2r wasn't made: " + why + ".\n" + rom->name + " is still there.");
        exit(1);
    }
    RemovePartials();
    fs::remove(rom->name, error);
    const long long seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    xsdk_log_write("first run: sf64.o2r made in %lld s; %s %s", seconds, rom->name.c_str(),
                   error ? "couldn't be deleted" : "deleted");
    const std::string removed = error ? rom->name + " couldn't be deleted; you can delete it yourself."
                                      : rom->name + " was deleted: Starship doesn't need it now, and you can copy it "
                                                    "again any time.";
    const auto shown = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - shown < std::chrono::milliseconds(kDoneShowMs)) {
        screen.Draw("sf64.o2r is made", removed + "\n\nStarting Starship...", 1.0f);
    }
    xsdk_present_close();
    xsdk_launch(ExLoadedImageName, nullptr, 0);
    exit(0);
}
