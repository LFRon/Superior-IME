// SPDX-License-Identifier: GPL-3.0-or-later
#include "superior/speech_recognition/Models.hpp"

#include <array>
#include <fstream>
#include <system_error>

namespace superior::speech_recognition {

namespace fs = std::filesystem;

namespace {

/// A GGUF file starts with these four bytes. Everything the catalogue lists is a
/// GGUF container, so the magic is a cheap and complete answer to "is this really
/// the model".
constexpr std::array<char, 4> kGgufMagic{'G', 'G', 'U', 'F'};

} // namespace

ModelFileCheck checkModelFile(const fs::path &path, std::uint64_t expectedBytes) {
    std::error_code error;
    if (path.empty() || !fs::is_regular_file(path, error)) {
        return ModelFileCheck::Missing;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return ModelFileCheck::Missing;
    }
    std::array<char, 4> magic{};
    file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (file.gcount() != static_cast<std::streamsize>(magic.size())) {
        return ModelFileCheck::NotGguf;
    }
    for (std::size_t index = 0; index < kGgufMagic.size(); ++index) {
        if (magic[index] != kGgufMagic[index]) {
            return ModelFileCheck::NotGguf;
        }
    }

    // The catalogue's byte count is a published approximation, not a checksum, so
    // a gross shortfall is the only honest signal. Half is far enough below any
    // real quantisation that a legitimate file cannot trip it.
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        return ModelFileCheck::Missing;
    }
    if (expectedBytes > 0 && size < expectedBytes / 2) {
        return ModelFileCheck::TooSmall;
    }
    return ModelFileCheck::Ok;
}

} // namespace superior::speech_recognition
