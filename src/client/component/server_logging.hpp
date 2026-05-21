#pragma once

#include <cstdint>

namespace server_logging
{
    bool open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id);
    bool has_session_key();
}
