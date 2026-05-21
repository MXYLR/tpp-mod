#pragma once

namespace game
{
	struct ISteamFriends;
}

namespace patches
{
	int get_real_friend_count(game::ISteamFriends* steam_friends, int eFriendFlags);
}
