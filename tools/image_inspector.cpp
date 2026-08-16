// DimOS build artifact validator.
//
// This host-side tool keeps binary-format checks out of the shell build script.
// It validates the boot sector, the kernel loader limit, the FAT12 geometry, and
// the KERNEL.BIN root-directory entry before an ISO is published.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kBootSectorSize = 512;
constexpr std::size_t kFloppySize = 1'474'560;
constexpr std::size_t kMaximumKernelSize = 43'008;
constexpr std::array<std::uint8_t, 11> kKernelFatName = {
    'K', 'E', 'R', 'N', 'E', 'L', ' ', ' ', 'B', 'I', 'N'};

using Bytes = std::vector<std::uint8_t>;

[[nodiscard]] Bytes read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("cannot open " + path);
    }

    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("cannot determine the size of " + path);
    }

    Bytes data(static_cast<std::size_t>(end));
    input.seekg(0, std::ios::beg);
    if (!data.empty() &&
        !input.read(reinterpret_cast<char*>(data.data()),
                    static_cast<std::streamsize>(data.size()))) {
        throw std::runtime_error("cannot read " + path);
    }
    return data;
}

[[nodiscard]] std::uint16_t read_u16(const Bytes& data, const std::size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("unexpected end of image while reading a 16-bit value");
    }
    return static_cast<std::uint16_t>(data[offset]) |
           (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(const Bytes& data, const std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("unexpected end of image while reading a 32-bit value");
    }
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

void require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

[[nodiscard]] std::string number(const std::size_t value) {
    std::ostringstream output;
    output << value;
    return output.str();
}

void validate_bootloader(const Bytes& bootloader) {
    require(bootloader.size() == kBootSectorSize,
            "BOOT.BIN must be exactly 512 bytes; got " + number(bootloader.size()));
    require(bootloader[510] == 0x55 && bootloader[511] == 0xAA,
            "BOOT.BIN does not end with the BIOS signature 0x55AA");
}

void validate_kernel(const Bytes& kernel) {
    require(!kernel.empty(), "KERNEL.BIN is empty");
    require(kernel.size() <= kMaximumKernelSize,
            "KERNEL.BIN exceeds the 43008-byte loader window; got " +
                number(kernel.size()));
}

void validate_geometry(const Bytes& image) {
    require(image.size() == kFloppySize,
            "floppy image must be exactly 1474560 bytes; got " + number(image.size()));
    require(read_u16(image, 11) == 512, "FAT12 bytes-per-sector field must be 512");
    require(image[13] == 1, "FAT12 sectors-per-cluster field must be 1");
    require(read_u16(image, 14) == 1, "FAT12 reserved-sector count must be 1");
    require(image[16] == 2, "FAT12 image must contain two FAT copies");
    require(read_u16(image, 17) == 224, "FAT12 root-directory entry count must be 224");
    require(read_u16(image, 19) == 2880, "FAT12 total-sector count must be 2880");
    require(image[21] == 0xF0, "FAT12 media descriptor must be 0xF0");
    require(read_u16(image, 22) == 9, "FAT12 sectors-per-FAT field must be 9");
    require(image[510] == 0x55 && image[511] == 0xAA,
            "floppy image does not contain the BIOS signature 0x55AA");
}

void validate_installed_bootloader(const Bytes& bootloader, const Bytes& image) {
    require(std::equal(bootloader.begin(), bootloader.end(), image.begin()),
            "the floppy boot sector differs from BOOT.BIN");
}

void validate_kernel_directory_entry(const Bytes& image, const Bytes& kernel) {
    const auto bytes_per_sector = static_cast<std::size_t>(read_u16(image, 11));
    const auto reserved_sectors = static_cast<std::size_t>(read_u16(image, 14));
    const auto fat_count = static_cast<std::size_t>(image[16]);
    const auto root_entry_count = static_cast<std::size_t>(read_u16(image, 17));
    const auto sectors_per_fat = static_cast<std::size_t>(read_u16(image, 22));
    const auto root_offset =
        (reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector;

    require(root_offset + root_entry_count * 32 <= image.size(),
            "FAT12 root directory extends beyond the image");

    for (std::size_t entry = 0; entry < root_entry_count; ++entry) {
        const auto offset = root_offset + entry * 32;
        if (image[offset] == 0x00) {
            break;
        }
        if (image[offset] == 0xE5 || image[offset + 11] == 0x0F) {
            continue;
        }

        if (std::equal(kKernelFatName.begin(), kKernelFatName.end(),
                       image.begin() + static_cast<std::ptrdiff_t>(offset))) {
            const auto recorded_size = read_u32(image, offset + 28);
            require(recorded_size == kernel.size(),
                    "KERNEL.BIN directory size does not match the compiled kernel");
            require(read_u16(image, offset + 26) >= 2,
                    "KERNEL.BIN has an invalid first cluster");
            return;
        }
    }

    throw std::runtime_error("KERNEL.BIN is missing from the FAT12 root directory");
}

void print_usage(const char* executable) {
    std::cerr << "Usage: " << executable
              << " <BOOT.BIN> <KERNEL.BIN> <dimos.img>\n";
}

}  // namespace

int main(const int argc, char* argv[]) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        const auto bootloader = read_file(argv[1]);
        const auto kernel = read_file(argv[2]);
        const auto image = read_file(argv[3]);

        validate_bootloader(bootloader);
        validate_kernel(kernel);
        validate_geometry(image);
        validate_installed_bootloader(bootloader, image);
        validate_kernel_directory_entry(image, kernel);

        std::cout << "Validated DimOS artifacts: boot=" << bootloader.size()
                  << " bytes, kernel=" << kernel.size()
                  << " bytes, floppy=" << image.size() << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "image_inspector: " << error.what() << '\n';
        return 1;
    }
}
