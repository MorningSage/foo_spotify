#pragma once

#include <qwr/fb2k_config.h>

namespace sptf::config
{

enum class BitrateSettings : uint8_t
{ // values are the same as in libspotify's sp_bitrate
    Bitrate160k = 0,
    Bitrate320k = 1,
    Bitrate96k = 2
};

extern qwr::fb2k::ConfigUint8Enum_MT<BitrateSettings> preferred_bitrate;

} // namespace sptf::config