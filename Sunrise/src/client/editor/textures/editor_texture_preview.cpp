#include "editor_texture_preview.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgiformat.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "../catalog/editor_asset_catalog.h"
#include "../../content/packages/package_keys.h"
#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::editor::textures {
namespace {

namespace package_reader = middleware::content::packages::reader;
constexpr std::size_t kCacheCapacity = 128;
constexpr unsigned kLoadsPerFrame = 2;
constexpr std::size_t kMaximumPayloadBytes = 128U * 1024U * 1024U;
constexpr std::uint16_t kCafe = 0xCAFEU;

struct CacheEntry {
    std::uint32_t tag{};
    std::uint64_t generation{};
    std::uint64_t used{};
    ID3D11Texture2D* texture{};
    Preview preview{};
    bool occupied{};
};

std::array<CacheEntry, kCacheCapacity> g_cache{};
package_reader::Scratch g_scratch{};
std::uint64_t g_useCounter{};
std::uint64_t g_catalogGeneration{};
unsigned g_remainingLoads{};
Preview g_deferred{};
ID3D11Device* g_device{};

template <typename T> void release_com(T*& value) noexcept {
    if (value != nullptr) { value->Release(); value = nullptr; }
}

void clear_entry(CacheEntry& entry) noexcept {
    release_com(entry.preview.view);
    release_com(entry.texture);
    entry = {};
}

template <typename T>
[[nodiscard]] bool load_le(std::span<const std::byte> bytes, std::size_t offset, T& value) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}

[[nodiscard]] bool parse_header(std::span<const std::byte> bytes, Metadata& output) noexcept {
    output = {};
    std::uint16_t cafePost = 0;
    std::uint16_t cafePre = 0;
    (void)load_le(bytes, 0x20, cafePost);
    (void)load_le(bytes, 0x0C, cafePre);
    std::size_t base = 0;
    if (cafePost == kCafe) {
        base = 0x20;
        output.preBeyondLightLayout = false;
    } else if (cafePre == kCafe) {
        base = 0x0C;
        output.preBeyondLightLayout = true;
    } else {
        return false;
    }
    if (!load_le(bytes, 0x00, output.dataSize) || !load_le(bytes, 0x04, output.dxgiFormat)
        || !load_le(bytes, base + 0x02, output.width)
        || !load_le(bytes, base + 0x04, output.height)
        || !load_le(bytes, base + 0x06, output.depth)
        || !load_le(bytes, base + 0x08, output.arraySize)) {
        return false;
    }
    const std::size_t largeOffset = output.preBeyondLightLayout ? 0x24 : 0x3C;
    if (!load_le(bytes, largeOffset, output.largeBufferTag)) return false;
    return output.width != 0 && output.height != 0;
}

[[nodiscard]] DXGI_FORMAT view_format(std::uint32_t raw) noexcept {
    switch (static_cast<DXGI_FORMAT>(raw)) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
        default: return static_cast<DXGI_FORMAT>(raw);
    }
}

struct Pitch { UINT row{}; UINT slice{}; };
[[nodiscard]] bool pitch_for(DXGI_FORMAT format, UINT width, UINT height, Pitch& output) noexcept {
    UINT blockBytes = 0;
    switch (format) {
        case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM: blockBytes = 8; break;
        case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: blockBytes = 16; break;
        default: break;
    }
    if (blockBytes != 0) {
        const UINT bw = (std::max)(1U, (width + 3U) / 4U);
        const UINT bh = (std::max)(1U, (height + 3U) / 4U);
        output.row = bw * blockBytes;
        output.slice = output.row * bh;
        return true;
    }
    UINT bytesPerPixel = 0;
    switch (format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT: bytesPerPixel = 16; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: bytesPerPixel = 8; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT:
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM: case DXGI_FORMAT_R8G8B8A8_SINT:
        case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT: case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: bytesPerPixel = 4; break;
        case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: bytesPerPixel = 2; break;
        case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM: bytesPerPixel = 1; break;
        default: return false;
    }
    output.row = width * bytesPerPixel;
    output.slice = output.row * height;
    return true;
}

class BlockKeyScope final {
public:
    package_reader::BlockKeys keys{};

    ~BlockKeyScope() noexcept { SecureZeroMemory(&keys, sizeof keys); }

    BlockKeyScope() = default;
    BlockKeyScope(const BlockKeyScope&) = delete;
    BlockKeyScope& operator=(const BlockKeyScope&) = delete;
};

[[nodiscard]] PreviewStatus load(CacheEntry& entry,
                                 ID3D11Device* device,
                                 std::uint32_t headerTag,
                                 std::uint32_t dataTag) noexcept {
    if (device == nullptr || catalog::package_directory_wide().empty()) return PreviewStatus::deviceCreateFailed;

    BlockKeyScope keyScope;
    if (!content::packages::collect_block_keys(keyScope.keys)) {
        return PreviewStatus::packageKeysUnavailable;
    }
    const package_reader::Source source{catalog::package_directory_wide(), &keyScope.keys};

    std::vector<std::byte> headerBytes;
    if (!package_reader::read_tag(source, g_scratch, headerTag, headerBytes)) {
        return PreviewStatus::headerReadFailed;
    }
    Metadata meta{};
    if (!parse_header(headerBytes, meta)) return PreviewStatus::headerInvalid;
    meta.dataTag = dataTag;
    if (meta.largeBufferTag != 0xFFFFFFFFU && meta.largeBufferTag >= 0x80800000U) {
        meta.dataTag = meta.largeBufferTag;
    }
    std::vector<std::byte> data;
    if (!package_reader::read_tag(source, g_scratch, meta.dataTag, data) || data.empty()
        || data.size() > kMaximumPayloadBytes) {
        return PreviewStatus::dataReadFailed;
    }

    const DXGI_FORMAT format = view_format(meta.dxgiFormat);
    Pitch pitch{};
    if (!pitch_for(format, meta.width, meta.height, pitch) || pitch.slice == 0 || data.size() < pitch.slice) {
        return PreviewStatus::unsupportedFormat;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = meta.width;
    desc.Height = meta.height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA initial{data.data(), pitch.row, pitch.slice};
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &texture)) || texture == nullptr
        || FAILED(device->CreateShaderResourceView(texture, nullptr, &view)) || view == nullptr) {
        release_com(view); release_com(texture); return PreviewStatus::deviceCreateFailed;
    }
    entry.texture = texture;
    entry.preview.metadata = meta;
    entry.preview.view = view;
    return PreviewStatus::ready;
}

[[nodiscard]] CacheEntry& slot_for(std::uint32_t tag) noexcept {
    CacheEntry* freeSlot = nullptr;
    CacheEntry* oldest = &g_cache.front();
    for (CacheEntry& entry : g_cache) {
        if (entry.occupied && entry.tag == tag && entry.generation == g_catalogGeneration) return entry;
        if (!entry.occupied && freeSlot == nullptr) freeSlot = &entry;
        if (entry.used < oldest->used) oldest = &entry;
    }
    CacheEntry* chosen = freeSlot != nullptr ? freeSlot : oldest;
    clear_entry(*chosen);
    chosen->occupied = true;
    chosen->tag = tag;
    chosen->generation = g_catalogGeneration;
    chosen->preview.status = PreviewStatus::notLoaded;
    return *chosen;
}

} // namespace

void begin_frame(std::uint64_t catalogGeneration) noexcept {
    if (g_catalogGeneration != catalogGeneration) {
        release_previews();
        g_catalogGeneration = catalogGeneration;
    }
    g_remainingLoads = kLoadsPerFrame;
}

const Preview& request(ID3D11Device* device,
                       std::uint32_t textureHeaderTag,
                       std::uint32_t textureDataTag) noexcept {
    if (device != g_device) {
        release_previews();
        g_device = device;
    }
    CacheEntry& entry = slot_for(textureHeaderTag);
    entry.used = ++g_useCounter;
    if (entry.preview.status == PreviewStatus::notLoaded && g_remainingLoads != 0) {
        --g_remainingLoads;
        entry.preview.status = load(entry, device, textureHeaderTag, textureDataTag);
    }
    if (entry.preview.status == PreviewStatus::notLoaded) {
        g_deferred = {};
        return g_deferred;
    }
    return entry.preview;
}

void release_previews() noexcept {
    for (CacheEntry& entry : g_cache) clear_entry(entry);
    package_reader::close_files(g_scratch);
    g_useCounter = 0;
    g_device = nullptr;
}

std::string_view status_name(PreviewStatus status) noexcept {
    switch (status) {
        case PreviewStatus::notLoaded: return "Queued";
        case PreviewStatus::ready: return "Ready";
        case PreviewStatus::packageKeysUnavailable: return "Package keys unavailable";
        case PreviewStatus::headerReadFailed: return "Header read failed";
        case PreviewStatus::headerInvalid: return "Header invalid";
        case PreviewStatus::dataReadFailed: return "Texture data read failed";
        case PreviewStatus::unsupportedFormat: return "Unsupported preview format/data layout";
        case PreviewStatus::deviceCreateFailed: return "D3D11 preview creation failed";
    }
    return "Unknown";
}

std::string_view format_name(std::uint32_t raw) noexcept {
    switch (static_cast<DXGI_FORMAT>(raw)) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        default: return "DXGI";
    }
}

} // namespace sunrise::client::editor::textures
