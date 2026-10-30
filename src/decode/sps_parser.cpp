#include "duocapture/decode/sps_parser.h"

#include <vector>

namespace duo {
namespace {

// 按位读取器。SPS 里的宽高是 Exp-Golomb 编码。
class BitReader {
 public:
  explicit BitReader(ByteSpan data) : data_(data) {}

  bool ok() const noexcept { return !overflow_; }

  u32 read_bits(int count) {
    u32 value = 0;
    for (int i = 0; i < count; ++i) {
      value = (value << 1) | read_bit();
    }
    return value;
  }

  // 无符号 Exp-Golomb
  u32 read_ue() {
    int leading_zeros = 0;
    while (read_bit() == 0) {
      if (++leading_zeros > 31 || overflow_) {
        overflow_ = true;
        return 0;
      }
    }
    if (leading_zeros == 0) {
      return 0;
    }
    const u32 suffix = read_bits(leading_zeros);
    return static_cast<u32>((1ull << leading_zeros) - 1 + suffix);
  }

  // 有符号 Exp-Golomb
  i32 read_se() {
    const u32 code = read_ue();
    const i32 half = static_cast<i32>((code + 1) / 2);
    return (code & 1) ? half : -half;
  }

  void skip_bits(int count) { (void)read_bits(count); }

 private:
  u32 read_bit() {
    if (bit_pos_ / 8 >= data_.size()) {
      overflow_ = true;
      return 0;
    }
    const u8 byte = data_[bit_pos_ / 8];
    const u8 bit = static_cast<u8>((byte >> (7 - (bit_pos_ % 8))) & 1);
    ++bit_pos_;
    return bit;
  }

  ByteSpan data_;
  std::size_t bit_pos_ = 0;
  bool overflow_ = false;
};

}  // namespace

Bytes remove_emulation_prevention(ByteSpan data) {
  Bytes out;
  out.reserve(data.size());
  int zeros = 0;
  for (const u8 byte : data) {
    if (zeros >= 2 && byte == 0x03) {
      zeros = 0;
      continue;  // 丢掉这个防竞争字节
    }
    out.push_back(byte);
    zeros = (byte == 0) ? zeros + 1 : 0;
  }
  return out;
}

SpsInfo parse_h264_sps(ByteSpan rbsp) {
  SpsInfo info{};
  BitReader reader(rbsp);

  const u32 profile_idc = reader.read_bits(8);
  reader.skip_bits(16);  // constraint flags + level_idc
  reader.read_ue();      // seq_parameter_set_id

  u32 chroma_format_idc = 1;
  if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
      profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
      profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
      profile_idc == 135) {
    chroma_format_idc = reader.read_ue();
    if (chroma_format_idc == 3) {
      reader.skip_bits(1);  // separate_colour_plane_flag
    }
    reader.read_ue();  // bit_depth_luma_minus8
    reader.read_ue();  // bit_depth_chroma_minus8
    reader.skip_bits(1);  // qpprime_y_zero_transform_bypass_flag
    if (reader.read_bits(1)) {  // seq_scaling_matrix_present_flag
      const int count = (chroma_format_idc != 3) ? 8 : 12;
      for (int i = 0; i < count; ++i) {
        if (!reader.read_bits(1)) {
          continue;
        }
        // 缩放列表：跳过比较麻烦，这里直接放弃解析
        return info;
      }
    }
  }

  reader.read_ue();  // log2_max_frame_num_minus4
  const u32 pic_order_cnt_type = reader.read_ue();
  if (pic_order_cnt_type == 0) {
    reader.read_ue();  // log2_max_pic_order_cnt_lsb_minus4
  } else if (pic_order_cnt_type == 1) {
    reader.skip_bits(1);  // delta_pic_order_always_zero_flag
    reader.read_se();     // offset_for_non_ref_pic
    reader.read_se();     // offset_for_top_to_bottom_field
    const u32 cycle = reader.read_ue();
    if (cycle > 255) {
      return info;
    }
    for (u32 i = 0; i < cycle; ++i) {
      reader.read_se();
    }
  }

  reader.read_ue();  // max_num_ref_frames
  reader.skip_bits(1);  // gaps_in_frame_num_value_allowed_flag

  const u32 width_in_mbs = reader.read_ue() + 1;
  const u32 height_in_map_units = reader.read_ue() + 1;
  const u32 frame_mbs_only = reader.read_bits(1);
  if (!frame_mbs_only) {
    reader.skip_bits(1);  // mb_adaptive_frame_field_flag
  }
  reader.skip_bits(1);  // direct_8x8_inference_flag

  u32 crop_left = 0;
  u32 crop_right = 0;
  u32 crop_top = 0;
  u32 crop_bottom = 0;
  if (reader.read_bits(1)) {  // frame_cropping_flag
    crop_left = reader.read_ue();
    crop_right = reader.read_ue();
    crop_top = reader.read_ue();
    crop_bottom = reader.read_ue();
  }

  if (!reader.ok()) {
    return info;
  }

  // 色度采样决定了裁剪单位
  u32 chroma_array_type = chroma_format_idc;
  if (chroma_array_type == 0) {
    chroma_array_type = 1;  // 4:0:0 时按 4:2:0 的裁剪单位处理
  }
  const u32 sub_width_c = (chroma_array_type == 1 || chroma_array_type == 2) ? 2 : 1;
  const u32 sub_height_c = (chroma_array_type == 1) ? 2 : 1;

  const u32 crop_unit_x = sub_width_c;
  const u32 crop_unit_y = sub_height_c * (2 - frame_mbs_only);

  info.width = width_in_mbs * 16 - (crop_left + crop_right) * crop_unit_x;
  info.height = (2 - frame_mbs_only) * height_in_map_units * 16 -
                (crop_top + crop_bottom) * crop_unit_y;
  info.valid = info.width != 0 && info.height != 0;
  return info;
}

SpsInfo parse_hevc_sps(ByteSpan rbsp) {
  SpsInfo info{};
  BitReader reader(rbsp);

  reader.skip_bits(4);                        // sps_video_parameter_set_id
  const u32 max_sub_layers_minus1 = reader.read_bits(3);
  reader.skip_bits(1);                        // sps_temporal_id_nesting_flag

  // profile_tier_level：general 部分固定 12 字节
  reader.skip_bits(8 * 12);

  // 子层数量 = max_sub_layers_minus1 + 1（不是 max_sub_layers_minus1）。
  // 少读这一位会让后面所有 Exp-Golomb 字段整体错位，宽高解析成垃圾值。
  const u32 sub_layers = max_sub_layers_minus1 + 1;
  std::vector<bool> profile_present(sub_layers, false);
  std::vector<bool> level_present(sub_layers, false);
  for (u32 i = 1; i < sub_layers; ++i) {
    profile_present[i] = reader.read_bits(1) != 0;
    level_present[i] = reader.read_bits(1) != 0;
  }
  if (sub_layers > 1) {
    for (u32 i = sub_layers; i < 8; ++i) {
      reader.skip_bits(2);  // reserved_zero_2bits
    }
  }
  for (u32 i = 1; i < sub_layers; ++i) {
    if (profile_present[i]) {
      reader.skip_bits(8 * 11);  // 子层 profile 字段（与 general 同构，88 位）
    }
    if (level_present[i]) {
      reader.skip_bits(8);
    }
  }

  reader.read_ue();  // sps_seq_parameter_set_id
  const u32 chroma_format_idc = reader.read_ue();
  if (chroma_format_idc == 3) {
    reader.skip_bits(1);  // separate_colour_plane_flag
  }
  const u32 width = reader.read_ue();
  const u32 height = reader.read_ue();

  u32 crop_left = 0;
  u32 crop_right = 0;
  u32 crop_top = 0;
  u32 crop_bottom = 0;
  if (reader.read_bits(1)) {  // conformance_window_flag
    crop_left = reader.read_ue();
    crop_right = reader.read_ue();
    crop_top = reader.read_ue();
    crop_bottom = reader.read_ue();
  }

  if (!reader.ok() || width == 0 || height == 0) {
    return info;
  }

  const u32 sub_width_c = (chroma_format_idc == 1 || chroma_format_idc == 2) ? 2 : 1;
  const u32 sub_height_c = (chroma_format_idc == 1) ? 2 : 1;

  info.width = width - (crop_left + crop_right) * sub_width_c;
  info.height = height - (crop_top + crop_bottom) * sub_height_c;
  info.valid = info.width != 0 && info.height != 0;
  return info;
}

SpsInfo parse_sps_dimensions(ByteSpan annex_b, Codec codec) {
  // 先切出所有 NAL 的边界，再逐段处理。用「往前找下一个起始码」的写法
  // 会在边界处多带字节进 RBSP，导致 Exp-Golomb 解析整体错位
  // ——实测会把 2560x1600 解析成 16x12。
  struct Nal {
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Nal> nals;
  for (std::size_t i = 0; i + 4 <= annex_b.size(); ++i) {
    if (annex_b[i] == 0 && annex_b[i + 1] == 0 && annex_b[i + 2] == 0 && annex_b[i + 3] == 1) {
      nals.push_back({i, annex_b.size()});
      if (nals.size() >= 2) {
        nals[nals.size() - 2].end = i;
      }
      i += 3;
    }
  }

  for (const Nal& nal : nals) {
    if (nal.begin + 5 >= nal.end) {
      continue;
    }
    const u8 header = annex_b[nal.begin + 4];
    const bool is_sps = codec == Codec::H264 ? ((header & 0x1F) == 7)
                                             : (((header >> 1) & 0x3F) == 33);
    if (!is_sps) {
      continue;
    }

    // NAL 头长度：H.264 是 1 字节，HEVC 是 2 字节（16 位 nal_unit_header）。
    // 少跳这一字节会让后面的 Exp-Golomb 全部错位 —— HEVC 会解析出
    // width=0、height=5121 这种明显不合理的值。
    const std::size_t header_len = codec == Codec::H264 ? 1 : 2;
    const std::size_t rbsp_begin = nal.begin + 4 + header_len;
    if (rbsp_begin >= nal.end) {
      continue;
    }

    // 注意：必须把 remove_emulation_prevention 的返回值存成具名 Bytes。
    // ByteSpan = std::span<const u8> 可以绑定到临时 vector（元素是 const，
    // 编译器不报错），但表达式一结束临时对象就析构，span 随即悬垂——
    // 实测表现为 SPS 解析出 profile=221 这种离谱值。
    const Bytes rbsp = remove_emulation_prevention(
        annex_b.subspan(rbsp_begin, nal.end - rbsp_begin));
    return codec == Codec::H264 ? parse_h264_sps(ByteSpan(rbsp.data(), rbsp.size()))
                                : parse_hevc_sps(ByteSpan(rbsp.data(), rbsp.size()));
  }

  return {};
}

}  // namespace duo
