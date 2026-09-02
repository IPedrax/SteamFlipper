#include "include/ByteSearch.h"

#include "include/Log.h"
#include "include/Stopwatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>

namespace SFPlatform::ByteSearch {
namespace {

class FixedPatternScanner {
public:
    explicit FixedPatternScanner(std::span<const uint8_t> pattern)
        : pattern_(pattern.begin(), pattern.end()) {
        const size_t patternSize = pattern_.size();
        skip_.fill(patternSize == 0 ? 1 : patternSize);
        if (patternSize <= 1) return;

        for (size_t i = 0; i + 1 < patternSize; ++i) {
            skip_[pattern_[i]] = patternSize - 1 - i;
        }
    }

    std::optional<size_t> Find(std::span<const uint8_t> bytes) const {
        const size_t patternSize = pattern_.size();
        if (patternSize == 0 || bytes.size() < patternSize) return std::nullopt;

        size_t offset = 0;
        while (offset <= bytes.size() - patternSize) {
            const uint8_t tail = bytes[offset + patternSize - 1];
            if (tail == pattern_[patternSize - 1]) {
                size_t i = patternSize - 1;
                while (i > 0 && bytes[offset + i - 1] == pattern_[i - 1]) {
                    --i;
                }
                if (i == 0) return offset;
            }

            offset += skip_[tail];
        }

        return std::nullopt;
    }

    size_t PatternSize() const { return pattern_.size(); }

private:
    std::vector<uint8_t> pattern_;
    std::array<size_t, 256> skip_{};
};

} // namespace

std::optional<size_t> Find(std::span<const uint8_t> bytes, std::span<const uint8_t> pattern) {
    const Stopwatch scanTimer;
    const auto result = FixedPatternScanner(pattern).Find(bytes);
    SFP_LOG_DEBUG("ByteSearch::Find bytes={} pattern={} matched={} elapsed_ms={:.3f}",
                   bytes.size(), pattern.size(), result.has_value(), scanTimer.ElapsedMs());
    return result;
}

std::optional<uint64_t> FindInFile(
    const std::filesystem::path& path,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    return FindInFileRange(
        path, 0, (std::numeric_limits<uint64_t>::max)(), pattern, chunkBytes);
}

std::optional<uint64_t> FindInFileRange(
    const std::filesystem::path& path,
    uint64_t offset,
    uint64_t size,
    std::span<const uint8_t> pattern,
    size_t chunkBytes) {
    FixedPatternScanner scanner(pattern);
    const size_t patternSize = scanner.PatternSize();
    if (patternSize == 0 || size == 0) return std::nullopt;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        SFP_LOG_DEBUG("ByteSearch::FindInFileRange open failed for '{}'", path.string());
        return std::nullopt;
    }

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return std::nullopt;
    }

    const uint64_t fileSize = static_cast<uint64_t>(st.st_size);
    if (offset >= fileSize) {
        close(fd);
        return std::nullopt;
    }

    const uint64_t rangeEnd = offset + (std::min)(size, fileSize - offset);
    chunkBytes = (std::max)(chunkBytes, patternSize);
    const uint64_t stride = static_cast<uint64_t>(chunkBytes) - (patternSize - 1);

    std::vector<uint8_t> buffer(chunkBytes);
    uint64_t currentOffset = offset;

    while (currentOffset < rangeEnd) {
        const size_t toRead = static_cast<size_t>(std::min<uint64_t>(chunkBytes, rangeEnd - currentOffset));
        ssize_t bytesRead = pread(fd, buffer.data(), toRead, static_cast<off_t>(currentOffset));
        if (bytesRead <= 0) break;

        auto match = scanner.Find(std::span<const uint8_t>(buffer.data(), static_cast<size_t>(bytesRead)));
        if (match) {
            close(fd);
            return currentOffset + *match;
        }

        currentOffset += stride;
    }

    close(fd);
    return std::nullopt;
}

} // namespace SFPlatform::ByteSearch
