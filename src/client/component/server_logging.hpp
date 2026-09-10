#pragma once

#include <cstdint>
#include <optional>
#include <nlohmann/json.hpp>

namespace server_logging
{
    bool open_wormhole(std::uint64_t target_player_id, std::uint64_t my_player_id);
    bool has_session_key();
    std::uint64_t get_my_player_id_from_cache();

    // Send a command through the standalone TPP client (does not touch the
    // game's NclDaemon, safe to call from any thread). Returns the parsed
    // response body on success.
    std::optional<nlohmann::json> send_session_command(const nlohmann::json& cmd);
}
