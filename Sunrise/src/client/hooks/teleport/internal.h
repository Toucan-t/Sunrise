#pragma once

#include <cstddef>
#include <cstdint>

#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::teleport {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

inline constexpr std::uint32_t kHandleIndexMask = 0x1FFF;
inline constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;

inline constexpr std::size_t kCameraBlockStride = 0xC50;
inline constexpr std::size_t kCameraPositionX = 1428;
inline constexpr std::size_t kCameraForwardX = 1468;

inline constexpr std::size_t kPhysicsComponentObjectHandle = 44;
inline constexpr std::size_t kPhysicsComponentSuppress = 568;

inline constexpr std::size_t kBodyFlags = 76;
inline constexpr std::uint32_t kBodyActiveBit = 0x40;
inline constexpr std::size_t kBodyMotionType = 352;
inline constexpr std::size_t kPhysicsComponentBodyArray = 400;
inline constexpr std::size_t kPhysicsComponentBodyIndex = 516;
inline constexpr std::size_t kBodyEntryStride = 80;
inline constexpr std::size_t kBodyPointer = 32;

inline constexpr std::size_t kBodyPositionX = 448;
inline constexpr std::size_t kBodyVelocityX = 560;

/** Three floats make one position or velocity vector. */
inline constexpr std::size_t kVectorLanes = 3;
/** The camera basis is X forward, Z up. */
inline constexpr std::size_t kVerticalLane = 2;

} // namespace sunrise::client::hooks::teleport
