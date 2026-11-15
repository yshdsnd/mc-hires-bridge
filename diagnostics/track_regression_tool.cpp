#include "track_regression_compare.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void printUsage(const char* prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " --synthetic [--tsv | --json]\n"
              << "  " << prog << " <pcm_file> <rate> <bits> <expected_frames> [options]\n\n"
              << "Options:\n"
              << "  --scenario <text>      Scenario name (e.g. \"1. 24->16b Tail\")\n"
              << "  --transition <text>    Format transition (e.g. \"24b/48k -> 16b/48k\")\n"
              << "  --op <type>            Manual operation: None, Pause, Seek, TrackChange, Stop, Exit\n"
              << "  --input-ms <float>     Manual operation input time in ms\n"
              << "  --target <text>        Intended target / position / track\n"
              << "  --reflect-ms <float>   Actual reflection time in ms\n"
              << "  --stale-eos <int>      Count of stale EOS intrusions (default: 0)\n"
              << "  --tsv                  Output format in TSV\n"
              << "  --json                 Output format in JSON\n"
              << "  --notes <text>         Optional notes\n"
              << "  --help                 Show this help\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string arg1 = argv[1];
    if (arg1 == "--help" || arg1 == "-h") {
        printUsage(argv[0]);
        return 0;
    }

    bool outputTsv = false;
    bool outputJson = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--tsv") outputTsv = true;
        if (std::string(argv[i]) == "--json") outputJson = true;
    }

    if (arg1 == "--synthetic") {
        auto suite = trackRegression::runSyntheticRegressionSuite();
        if (outputTsv) {
            std::cout << trackRegression::tsvHeader();
            for (const auto& rec : suite) {
                std::cout << trackRegression::toTsvRow(rec);
            }
        } else if (outputJson) {
            std::cout << "[\n";
            for (size_t i = 0; i < suite.size(); ++i) {
                std::cout << trackRegression::toJson(suite[i]);
                if (i + 1 < suite.size()) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "]\n";
        } else {
            std::cout << "# Real-Machine Regression Synthetic Verification Suite\n\n";
            std::cout << trackRegression::markdownHeader();
            for (const auto& rec : suite) {
                std::cout << trackRegression::toMarkdownRow(rec);
            }
            std::cout << "\nSummary:\n";
            size_t passCount = 0;
            for (const auto& rec : suite) {
                if (rec.passed) ++passCount;
            }
            std::cout << "Total Cases: " << suite.size()
                      << " | Passed: " << passCount
                      << " | Expected Failures/Truncations Detected: " << (suite.size() - passCount) << "\n";
        }
        return 0;
    }

    if (argc < 5) {
        std::cerr << "Error: Missing required positional arguments.\n";
        printUsage(argv[0]);
        return 1;
    }

    std::string pcmPath = argv[1];
    uint32_t rate = static_cast<uint32_t>(std::stoul(argv[2]));
    uint16_t bits = static_cast<uint16_t>(std::stoul(argv[3]));
    size_t expectedFrames = static_cast<size_t>(std::stoull(argv[4]));

    std::string scenario = "Real-Machine Run";
    std::string transition = "-";
    std::string manualOp = "None";
    double inputTimeMs = 0.0;
    std::string target = "-";
    double reflectTimeMs = 0.0;
    uint32_t staleEos = 0;
    std::string notes = "";

    for (int i = 5; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--scenario" && i + 1 < argc) scenario = argv[++i];
        else if (a == "--transition" && i + 1 < argc) transition = argv[++i];
        else if (a == "--op" && i + 1 < argc) manualOp = argv[++i];
        else if (a == "--input-ms" && i + 1 < argc) inputTimeMs = std::stod(argv[++i]);
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--reflect-ms" && i + 1 < argc) reflectTimeMs = std::stod(argv[++i]);
        else if (a == "--stale-eos" && i + 1 < argc) staleEos = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--notes" && i + 1 < argc) notes = argv[++i];
    }

    std::ifstream file(pcmPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open PCM file: " << pcmPath << "\n";
        return 2;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Error: Failed to read PCM file: " << pcmPath << "\n";
        return 3;
    }

    trackRegression::Record rec = trackRegression::evaluatePcm(
        "CLI-RUN", scenario, transition,
        buffer.data(), buffer.size(),
        rate, bits, expectedFrames,
        manualOp, inputTimeMs, target, reflectTimeMs,
        staleEos, notes);

    if (outputTsv) {
        std::cout << trackRegression::tsvHeader();
        std::cout << trackRegression::toTsvRow(rec);
    } else if (outputJson) {
        std::cout << trackRegression::toJson(rec) << "\n";
    } else {
        std::cout << trackRegression::markdownHeader();
        std::cout << trackRegression::toMarkdownRow(rec);
    }

    return rec.passed ? 0 : 1;
}
