#pragma once

#include "media_pipeline.hpp"

#include <limits>
#include <string>

namespace cvi_media {

template <typename T>
bool parse_arg(const std::string &text, T &value);

template <>
inline bool parse_arg<CVI_U32>(const std::string &text, CVI_U32 &value) {
  if (text.empty() || text[0] == '-') {
    return false;
  }

  try {
    std::size_t consumed = 0;
    const unsigned long parsed = std::stoul(text, &consumed, 10);
    if (consumed != text.size() || parsed == 0 ||
        parsed > std::numeric_limits<CVI_U32>::max()) {
      return false;
    }
    value = static_cast<CVI_U32>(parsed);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

template <>
inline bool parse_arg<Codec>(const std::string &text, Codec &value) {
  if (text == "h264") {
    value = Codec::H264;
    return true;
  }
  if (text == "h265") {
    value = Codec::H265;
    return true;
  }
  return false;
}

}  // namespace cvi_media
