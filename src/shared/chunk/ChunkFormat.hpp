#pragma once
#include <cstdint>

// ============================================================================
// chunk 存档的格式常量（二进制布局见 docs\DOCS自制版\存档系统设计.md）
// ============================================================================
//
// 设计要点（与 LOD 的耦合）：
//   · 头固定 32 字节 + 段表（tag/len），正文按段排列 —— 将来加 LOD 摘要、光照占位、
//     方块实体都只是新增一个 tag，老读者按 len 跳过，**不需要升格式版本**。
//   · 只存体素真值；网格/LOD 是派生物，永不入库。
//   · checksum 覆盖"段表 + 正文"，读入时校验，防截断/写中断。
namespace engine::shared::chunkfmt {

// "CHNK" 的小端写法（写入时按 u32 小端存）
inline constexpr std::uint32_t kMagic = 0x4B4E4843u;

inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::uint16_t kHeaderSize    = 32;   // 固定头长度（预留扩容）

// 段 tag
inline constexpr std::uint16_t kTagPalette = 0x0001;  // count u16 + count × u16
inline constexpr std::uint16_t kTagIndices = 0x0002;  // bits u8 + pad u8 + len u32 + data

// flags 位
inline constexpr std::uint16_t kFlagAllAir = 0x0001;  // 全空气：无段，正文为空

inline constexpr std::uint16_t kMaxSectionCount = 16;

} // namespace engine::shared::chunkfmt
