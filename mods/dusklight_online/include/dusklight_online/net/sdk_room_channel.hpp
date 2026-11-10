#pragma once

#include "dusklight_online/net/transport.hpp"

#include <memory>

namespace dusklight_online::net {
std::unique_ptr<RoomChannel> make_sdk_room_channel();
}
