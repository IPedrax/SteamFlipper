#include "include/PE.h"

#include "include/Log.h"
#include "include/Stopwatch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace SFPlatform::PE {
namespace {

#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    int32_t  e_lfanew;
};

struct FileHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

// Named after the Win32 type (IMAGE_DATA_DIRECTORY) rather than the field it
// backs: a member called DataDirectory inside a struct whose scope also names
// the type DataDirectory changes the meaning of that name mid-class, which GCC
// rejects (-Wchanges-meaning).
struct ImageDataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};

struct OptionalHeader64 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    ImageDataDirectory DataDirectory[16];
};

struct OptionalHeader32 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    ImageDataDirectory DataDirectory[16];
};

struct SectionHeader {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};

struct ExportDirectory {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;
    uint32_t Base;
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;
    uint32_t AddressOfNames;
    uint32_t AddressOfNameOrdinals;
};
#pragma pack(pop)

constexpr size_t kHeaderReadBytes = 4 * 1024 * 1024;

} // namespace

bool Section::ContainsRva(uint32_t rva) const {
    const uint32_t span = (std::max)(virtualSize, rawSize);
    return rva >= virtualAddress && rva < virtualAddress + span;
}

Image::Image(const std::filesystem::path& path) : path_(path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;

    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return;
    }
    fileSize_ = static_cast<uint64_t>(st.st_size);

    const size_t toRead = static_cast<size_t>(std::min<uint64_t>(fileSize_, kHeaderReadBytes));
    headerBytes_ = ByteBuffer(toRead);
    ssize_t bytesRead = pread(fd, headerBytes_.data(), toRead, 0);
    close(fd);

    if (bytesRead < static_cast<ssize_t>(sizeof(DosHeader))) return;

    const auto* dos = reinterpret_cast<const DosHeader*>(headerBytes_.data());
    if (dos->e_magic != 0x5A4D /* MZ */ || dos->e_lfanew < 0) return;

    const size_t ntOffset = static_cast<size_t>(dos->e_lfanew);
    if (ntOffset + 4 + sizeof(FileHeader) > static_cast<size_t>(bytesRead)) return;

    const uint8_t* nt = headerBytes_.data() + ntOffset;
    if (memcmp(nt, "PE\0\0", 4) != 0) return;

    const auto* fileHdr = reinterpret_cast<const FileHeader*>(nt + 4);
    const uint8_t* optHdr = nt + 4 + sizeof(FileHeader);
    const uint16_t optMagic = *reinterpret_cast<const uint16_t*>(optHdr);

    const SectionHeader* sections = nullptr;
    if (optMagic == 0x20B /* PE32+ */) {
        const auto* opt64 = reinterpret_cast<const OptionalHeader64*>(optHdr);
        entryPointRva_ = opt64->AddressOfEntryPoint;
        if (opt64->NumberOfRvaAndSizes > 0) {
            exportDirectoryRva_ = opt64->DataDirectory[0].VirtualAddress;
            exportDirectorySize_ = opt64->DataDirectory[0].Size;
        }
        sections = reinterpret_cast<const SectionHeader*>(optHdr + fileHdr->SizeOfOptionalHeader);
    } else if (optMagic == 0x10B /* PE32 */) {
        const auto* opt32 = reinterpret_cast<const OptionalHeader32*>(optHdr);
        entryPointRva_ = opt32->AddressOfEntryPoint;
        if (opt32->NumberOfRvaAndSizes > 0) {
            exportDirectoryRva_ = opt32->DataDirectory[0].VirtualAddress;
            exportDirectorySize_ = opt32->DataDirectory[0].Size;
        }
        sections = reinterpret_cast<const SectionHeader*>(optHdr + fileHdr->SizeOfOptionalHeader);
    } else {
        return;
    }

    const uint8_t* secPtr = reinterpret_cast<const uint8_t*>(sections);
    const size_t secEnd = (secPtr - headerBytes_.data()) + fileHdr->NumberOfSections * sizeof(SectionHeader);
    if (secEnd > static_cast<size_t>(bytesRead)) return;

    sections_.reserve(fileHdr->NumberOfSections);
    for (uint16_t i = 0; i < fileHdr->NumberOfSections; ++i) {
        const auto& sh = sections[i];
        char nameBuf[9]{};
        memcpy(nameBuf, sh.Name, 8);

        Section s;
        s.name = nameBuf;
        s.virtualAddress = sh.VirtualAddress;
        s.virtualSize = sh.VirtualSize;
        s.rawOffset = sh.PointerToRawData;
        s.rawSize = sh.SizeOfRawData;
        s.characteristics = sh.Characteristics;
        sections_.push_back(std::move(s));
    }

    valid_ = true;
}

bool Image::HasSection(std::string_view name) const {
    for (const auto& s : sections_) {
        if (s.name == name) return true;
    }
    return false;
}

const Section* Image::SectionContainingRva(uint32_t rva) const {
    for (const auto& s : sections_) {
        if (s.ContainsRva(rva)) return &s;
    }
    return nullptr;
}

std::optional<size_t> Image::RvaToOffset(uint32_t rva) const {
    const Section* s = SectionContainingRva(rva);
    if (!s) return std::nullopt;
    return static_cast<size_t>(s->rawOffset + (rva - s->virtualAddress));
}

std::optional<uint32_t> Image::RawOffsetToRva(size_t rawOffset) const {
    for (const auto& s : sections_) {
        if (rawOffset >= s.rawOffset && rawOffset < s.rawOffset + s.rawSize) {
            return static_cast<uint32_t>(s.virtualAddress + (rawOffset - s.rawOffset));
        }
    }
    return std::nullopt;
}

ByteBuffer Image::ReadRawBytes(size_t offset, size_t size) const {
    if (size == 0 || offset >= fileSize_) return {};

    int fd = open(path_.c_str(), O_RDONLY);
    if (fd < 0) return {};

    const size_t toRead = std::min<size_t>(size, static_cast<size_t>(fileSize_ - offset));
    ByteBuffer buf(toRead);
    ssize_t bytesRead = pread(fd, buf.data(), toRead, static_cast<off_t>(offset));
    close(fd);

    if (bytesRead <= 0) return {};
    return buf;
}

ByteBuffer Image::ReadAllBytes() const {
    return ReadRawBytes(0, static_cast<size_t>(fileSize_));
}

std::optional<Export> Image::FindExport(std::string_view symbolName) const {
    if (exportDirectoryRva_ == 0 || exportDirectorySize_ == 0) return std::nullopt;

    auto offset = RvaToOffset(exportDirectoryRva_);
    if (!offset) return std::nullopt;

    ByteBuffer dirBuf = ReadRawBytes(*offset, sizeof(ExportDirectory));
    if (dirBuf.size() < sizeof(ExportDirectory)) return std::nullopt;

    const auto* exp = reinterpret_cast<const ExportDirectory*>(dirBuf.data());
    if (exp->NumberOfNames == 0) return std::nullopt;

    auto namesOffset = RvaToOffset(exp->AddressOfNames);
    auto ordinalsOffset = RvaToOffset(exp->AddressOfNameOrdinals);
    auto functionsOffset = RvaToOffset(exp->AddressOfFunctions);
    if (!namesOffset || !ordinalsOffset || !functionsOffset) return std::nullopt;

    ByteBuffer namesBuf = ReadRawBytes(*namesOffset, exp->NumberOfNames * sizeof(uint32_t));
    ByteBuffer ordBuf = ReadRawBytes(*ordinalsOffset, exp->NumberOfNames * sizeof(uint16_t));
    ByteBuffer fnBuf = ReadRawBytes(*functionsOffset, exp->NumberOfFunctions * sizeof(uint32_t));

    const auto* rvaNames = reinterpret_cast<const uint32_t*>(namesBuf.data());
    const auto* ordinals = reinterpret_cast<const uint16_t*>(ordBuf.data());
    const auto* functions = reinterpret_cast<const uint32_t*>(fnBuf.data());

    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        auto strOff = RvaToOffset(rvaNames[i]);
        if (!strOff) continue;

        ByteBuffer strBuf = ReadRawBytes(*strOff, 256);
        if (strBuf.empty()) continue;
        const char* name = reinterpret_cast<const char*>(strBuf.data());

        if (symbolName == name) {
            uint16_t ord = ordinals[i];
            if (ord < exp->NumberOfFunctions) {
                uint32_t fnRva = functions[ord];
                Export e;
                e.rva = fnRva;
                return e;
            }
        }
    }

    return std::nullopt;
}

} // namespace SFPlatform::PE
