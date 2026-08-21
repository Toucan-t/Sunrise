#pragma once

#include <cstdint>
#include <string_view>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace sunrise::client::editor::textures {

enum class PreviewStatus : std::uint8_t {
    notLoaded,
    ready,
    packageKeysUnavailable,
    headerReadFailed,
    headerInvalid,
    dataReadFailed,
    unsupportedFormat,
    deviceCreateFailed,
};

struct Metadata {
    std::uint32_t dataSize{};
    std::uint32_t dxgiFormat{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint16_t depth{};
    std::uint16_t arraySize{};
    std::uint32_t largeBufferTag{0xFFFFFFFFU};
    std::uint32_t dataTag{};
    bool preBeyondLightLayout{};
};

struct Preview {
    PreviewStatus status{PreviewStatus::notLoaded};
    Metadata metadata{};
    ID3D11ShaderResourceView* view{};
};

/** Begins one Editor UI frame and permits at most two new package-backed preview decodes. */
void begin_frame(std::uint64_t catalogGeneration) noexcept;
/** Returns a cached preview, lazily decoding it when this frame still has budget. */
[[nodiscard]] const Preview& request(ID3D11Device* device,
                                     std::uint32_t textureHeaderTag,
                                     std::uint32_t textureDataTag) noexcept;
/** Releases every Editor-owned preview SRV/texture. Must run before renderer device release. */
void release_previews() noexcept;
[[nodiscard]] std::string_view status_name(PreviewStatus status) noexcept;
[[nodiscard]] std::string_view format_name(std::uint32_t dxgiFormat) noexcept;

} // namespace sunrise::client::editor::textures
