#include "track_regression_compare.h"
#include "track_reference_compare.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void printUsage(const char* prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " --synthetic [--tsv | --json]\n"
              << "  " << prog << " --generate-fixtures <directory>\n"
              << "  " << prog << " --loopback <file> --ref1 <track1.wav> --ref2 <track2.wav> [options]\n"
              << "  " << prog << " <pcm_file> <rate> <bits> <expected_frames> [options]\n\n"
              << "Reference-Based Comparison Options (Issue #50):\n"
              << "  --loopback <path>      Path to loopback capture (192kHz stereo float32 raw or WAV)\n"
              << "  --ref1 <path>          Path to Track 1 reference WAV\n"
              << "  --ref2 <path>          Path to Track 2 reference WAV\n"
              << "  --loopback-rate <int>  Raw loopback sample rate (default: 192000)\n"
              << "  --loopback-ch <int>    Raw loopback channels (default: 2)\n"
              << "  --threshold <float>    Normalized cross-correlation threshold (default: 0.65)\n"
              << "  --template-ms <float>  Template length in ms (default: 250.0)\n"
              << "  --tail-tol-ms <float>  Tail completeness tolerance in ms (default: 50.0)\n\n"
              << "General Options:\n"
              << "  --scenario <text>      Scenario name (e.g. \"1. 24->16b Tail\")\n"
              << "  --transition <text>    Format transition (e.g. \"24b/48k -> 16b/48k\")\n"
              << "  --op <type>            Manual operation: None, Pause, Seek, TrackChange, Stop, Exit\n"
              << "  --input-ms <float>     Manual operation input time in ms\n"
              << "  --target <text>        Intended target / position / track\n"
              << "  --reflect-ms <float>   Actual reflection time in ms\n"
              << "  --stale-eos <int>      Count of stale EOS intrusions (default: 0)\n"
              << "  --discontinuity        Flag loopback discontinuity across track boundary (marks verdict as PENDING / 判定保留)\n"
              << "  --pending              Alias for --discontinuity\n"
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

    if (arg1 == "--generate-fixtures") {
        if (argc < 3) {
            std::cerr << "Error: Output directory required for --generate-fixtures.\n";
            return 1;
        }
        std::string outDir = argv[2];
        if (trackReference::generateFixtures(outDir)) {
            std::cout << "Successfully generated synthetic test fixtures in: " << outDir << "\n";
            return 0;
        } else {
            std::cerr << "Error: Failed to generate synthetic test fixtures in: " << outDir << "\n";
            return 2;
        }
    }

    if (arg1 == "--synthetic") {
        auto suite = trackRegression::runSyntheticRegressionSuite();
        trackReference::appendSyntheticReferenceTests(suite);

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
            size_t pendingCount = 0;
            for (const auto& rec : suite) {
                if (rec.verdict == "PASS") ++passCount;
                else if (rec.verdict == "PENDING") ++pendingCount;
            }
            std::cout << "Total Cases: " << suite.size()
                      << " | Passed: " << passCount
                      << " | Pending (判定保留): " << pendingCount
                      << " | Expected Failures/Truncations Detected: " << (suite.size() - passCount - pendingCount) << "\n";
        }
        return 0;
    }

    // Parse options
    std::string loopbackPath;
    std::string ref1Path;
    std::string ref2Path;
    uint32_t loopRate = 192000;
    uint16_t loopCh = 2;
    double threshold = 0.65;
    double templateMs = 250.0;
    double tailTolMs = 50.0;

    std::string scenario = "Real-Machine Run";
    std::string transition = "-";
    std::string manualOp = "None";
    double inputTimeMs = 0.0;
    std::string target = "-";
    double reflectTimeMs = 0.0;
    uint32_t staleEos = 0;
    std::string notes = "";
    bool loopbackDiscontinuity = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--loopback" && i + 1 < argc) loopbackPath = argv[++i];
        else if (a == "--ref1" && i + 1 < argc) ref1Path = argv[++i];
        else if (a == "--ref2" && i + 1 < argc) ref2Path = argv[++i];
        else if (a == "--loopback-rate" && i + 1 < argc) loopRate = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--loopback-ch" && i + 1 < argc) loopCh = static_cast<uint16_t>(std::stoul(argv[++i]));
        else if (a == "--threshold" && i + 1 < argc) threshold = std::stod(argv[++i]);
        else if (a == "--template-ms" && i + 1 < argc) templateMs = std::stod(argv[++i]);
        else if (a == "--tail-tol-ms" && i + 1 < argc) tailTolMs = std::stod(argv[++i]);
        else if (a == "--scenario" && i + 1 < argc) scenario = argv[++i];
        else if (a == "--transition" && i + 1 < argc) transition = argv[++i];
        else if (a == "--op" && i + 1 < argc) manualOp = argv[++i];
        else if (a == "--input-ms" && i + 1 < argc) inputTimeMs = std::stod(argv[++i]);
        else if (a == "--target" && i + 1 < argc) target = argv[++i];
        else if (a == "--reflect-ms" && i + 1 < argc) reflectTimeMs = std::stod(argv[++i]);
        else if (a == "--stale-eos" && i + 1 < argc) staleEos = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--notes" && i + 1 < argc) notes = argv[++i];
        else if (a == "--discontinuity" || a == "--pending") loopbackDiscontinuity = true;
    }

    // Reference-Based Comparison Mode (Issue #50)
    if (!loopbackPath.empty()) {
        if (ref1Path.empty() || ref2Path.empty()) {
            std::cerr << "Error: Both --ref1 and --ref2 must be provided with --loopback.\n";
            printUsage(argv[0]);
            return 1;
        }

        trackReference::AudioBuffer loopAudio;
        if (!trackReference::loadAudio(loopbackPath, loopRate, loopCh, loopAudio)) {
            std::cerr << "Error: Could not load loopback file: " << loopbackPath << "\n";
            return 2;
        }

        trackReference::AudioBuffer ref1Audio;
        if (!trackReference::readWav(ref1Path, ref1Audio)) {
            std::cerr << "Error: Could not read Track 1 reference WAV: " << ref1Path << "\n";
            return 2;
        }

        trackReference::AudioBuffer ref2Audio;
        if (!trackReference::readWav(ref2Path, ref2Audio)) {
            std::cerr << "Error: Could not read Track 2 reference WAV: " << ref2Path << "\n";
            return 2;
        }

        trackReference::MatchConfig cfg;
        cfg.correlationThreshold = threshold;
        cfg.templateDurationMs = templateMs;
        cfg.tailToleranceMs = tailTolMs;
        cfg.forcePending = loopbackDiscontinuity;

        trackReference::MatchResult m = trackReference::evaluateLoopback(loopAudio, ref1Audio, ref2Audio, cfg);
        if (!notes.empty()) {
            if (m.notes.empty()) m.notes = notes;
            else m.notes += " | " + notes;
        }

        trackRegression::Record rec = trackReference::toRegressionRecord(
            "CLI-REF", scenario, transition, m, loopRate, 32);

        if (outputTsv) {
            std::cout << trackRegression::tsvHeader();
            std::cout << trackRegression::toTsvRow(rec);
        } else if (outputJson) {
            std::cout << trackRegression::toJson(rec) << "\n";
        } else {
            std::cout << trackRegression::markdownHeader();
            std::cout << trackRegression::toMarkdownRow(rec);
        }

        return (rec.verdict == "FAIL") ? 1 : 0;
    }

    // Legacy positional comparison mode: <pcm_file> <rate> <bits> <expected_frames>
    if (argc < 5) {
        std::cerr << "Error: Missing required positional arguments or --loopback options.\n";
        printUsage(argv[0]);
        return 1;
    }

    std::string pcmPath = argv[1];
    uint32_t rate = static_cast<uint32_t>(std::stoul(argv[2]));
    uint16_t bits = static_cast<uint16_t>(std::stoul(argv[3]));
    size_t expectedFrames = static_cast<size_t>(std::stoull(argv[4]));

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
        staleEos, notes, loopbackDiscontinuity);

    if (outputTsv) {
        std::cout << trackRegression::tsvHeader();
        std::cout << trackRegression::toTsvRow(rec);
    } else if (outputJson) {
        std::cout << trackRegression::toJson(rec) << "\n";
    } else {
        std::cout << trackRegression::markdownHeader();
        std::cout << trackRegression::toMarkdownRow(rec);
    }

    return (rec.verdict == "FAIL") ? 1 : 0;
}
