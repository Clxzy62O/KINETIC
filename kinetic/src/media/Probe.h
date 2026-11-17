#pragma once
#include <string>

#include "project/Model.h"

namespace kinetic::media {

// Probes a file with ffprobe and fills a MediaRef (id is left as kNoId). Throws on unreadable media.
MediaRef probe(const std::string& path);

// True for image formats treated as stills (single frame, infinite duration).
bool isStillPath(const std::string& path);

} // namespace kinetic::media
