# tpp-mod

Improvements and fixes for Metal Gear Solid V: The Phantom Pain

discord server
https://discord.gg/hYfW9MEEGF

## Installation

tpp-mod only supports original steam releases (latest), any kind of modified or pirated version is **NOT** supported.   

| version | sha1 | support |
| --- | --- | --- |
| mgsvtpp.exe (eng, 1.0.15.3) | 17AC94A4BC9F88B035A45122C8A67EFA38D03F2A | ✔️ |
| mgsvmgo.exe (eng, 1.1.2.7) | 8690C7C27C94DD6A452BA6A612B7B485918F3BAF | ✔️ |
| mgsvtpp.exe (jpn, 1.0.15.3) | 3A87F626732158890A07688D32A2523CD8EADA71 | ✔️ |
| mgsvmgo.exe (jpn, 1.1.2.7) | D6ADF7685B0F0639B2A949D0E96A06E853DAEEB8 | ✔️ |

- Download [dinput8.dll](https://github.com/MXYLR/tpp-mod/releases) from the release page and copy it to the game folder.
- Download [fob-gui.exe](https://github.com/MXYLR/tpp-mod/releases) and place it in the game folder alongside dinput8.dll (optional, for the FOB GUI application).

Configuration files are stored in `%localappdata%\tpp-mod`

### FOB GUI Application

The `fob-gui.exe` is an optional companion application that launches automatically with the game (via dinput8.dll) and provides a graphical interface for FOB-related operations:

- **Wormhole management**: Enter target Steam ID and open wormholes to bypass FOB blockades
- **Command execution**: Click any available tpp-mod command from the list with descriptions
- **Console log viewer**: Real-time display of tpp-mod console logs with auto-scroll
- **List type conversion**: Popup dialog to convert FOB list types (PICKUP -> FOLLOW, etc.)
- **Cached players browser**: View all cached player info (name, Steam name, Steam ID, Player ID) with copy buttons
- **Custom targets browser**: View all custom FOB targets with copy buttons
- **Dynamic scaling**: Adjustable UI scale input box (0.5 - 5.0) for font and button sizes
- **Variables browser**: Categorized view of all tpp-mod variables with current values, descriptions, and quick-set buttons
- **All friends dialog**: Fetch and display all Steam friends (bypassing the in-game 50-player limit) with copy buttons
- **Maximize button**: Standard Windows maximize/restore button for switching between windowed and fullscreen

The GUI communicates with dinput8.dll via a named pipe, so both files must be in the same directory. The GUI automatically closes when the game exits.

## Features

| <img src="assets/github/1.png?raw=true" /> |
|:-:|
| in game text chat & ping/fps counter |

| <img src="assets/github/2.png?raw=true" /> |
|:-:|
| in game console |

- discord rich presence
- unlock fps and custom fps capping
- fps and ping counter
- mgo text chat
- in game console
- high fps wake up time patch
- custom security challenge list
- kick players from your mgo lobby
- DSX integration
- lua script overriding and custom script loading
- custom backend server support (see: tpp-server-emulator)
- mgo dedicated server support (experimental)
- custom key bindings
- key remapping
- fov scaling
- start mgo directly
- allow multiple instance of the game
- scale mouse sensitivity
- disable intro splashscreens
- **FOB custom targets and management**

### Command list
Can be executed from the console or in game console 

**Basic Commands**
- `bind <key> <command>`: bind a key to a command
- `remap <key from> <key to>`: remap a key to another
- `unbind <key>`: remove custom bind from key
- `unbindall`: remove all custom binds
- `exec <cfg name>`: execute a config file 
- `alias <name> <command>`: create a command alias 
- `help <command>`: show help for a command
- `clear`: clear the ingame console
- `toggleconsole`: toggle the ingame console
- `quit`: quit the game
- `startsound <id>`: start playing a sound
- `stopsound <id>`: stop playing the sound
- `wait <frame count>`: wait before executing the next command
- `startmgo`: start mgo
- `starttpp`: start tpp
- `dumpstrings`: dump computed hashes (requires -dump-hashes launch parameter)

**Equipment Commands**
- `equip <slot type> <index>`: equip a slot (slot types: 'primary1', 'primary2', 'secondary', 'support', 'item', 'stole', hand')
- `equiptoggle <slot type> <index 1> <index 2>`: toggle between 2 equip slots
- `equipnext <slot type>`: equip the next slot
- `equipprev <slot type>`: equip the previous slot

**Variable Commands**
- `set <var name> <value>`: set a var
- `reset <var name>`: reset a var
- `var_list`: show the list of vars

**Scripting Commands**
- `script_var <varname>`: view lua script var
- `script_exec <code>`: execute lua code
- `script_load <path>`: load a lua script

**Session/Lobby Commands**
- `disconnect`: return to acc/leave the mgo lobby
- `status`: return list of players in session
- `rtt`: connection rtt in milliseconds
- `session_create`: create a session
- `session_start`: start the session
- `session_accept`: accepts new session connections
- `session_close`: close the session
- `session_connect <steamid>`: connect to a session
- `clearkicks`: clear the kicked players list
- `connect_lobby <lobbyid>`: connect to a lobby
- `reconnect`: disconnect and reconnects to the current lobby
- `kick <name|steamid|index>`: kick a player from the lobby

**Match Commands**
- `matchstart`: start a match
- `matchrotate`: rotate the match to the next slot
- `matchset <name> <value>`: set a match setting
- `matchsetrule <name> <value>`: set a match rule
- `matchsetslot <slot> <name> <value>`: set a match' slot rule
- `matchsetstate <state> <param>`: set match state
- `matchprint`: print current match settings

**Chat Commands**
- `clearchat`: clear the chat
- `chatall`: open the all chat
- `chatteam`: open the team chat
- `say <message>`: send a message to all chat
- `say_team <message>`: send a message to team chat
- `mute <name|steamid|index>`: mute a player
- `unmute <name|steamid|index>`: unmute a player
- `mutelist`: show mutes players
- `clearmutes`: unmute all players

**FOB Commands**
- `fob_add_support <steam_id>`: send CMD_ADD_FOLLOW request to add a player as support (use in Relationships -> Friends list -> click Support on any player to trigger)
- `fob_add_target <steam_id>/<player_id>/<mother_base_id>`: add a custom FOB target with player ID or mother base ID or steam id.
- `fob_remove_target <steam_id>`: remove a custom FOB target
- `fob_clear_targets`: clear all custom FOB targets
- `fob_target_list`: list all custom FOB targets
- `fob_cache_list`: list all cached FOB targets (auto-populated when browsing FOB lists)
- `fob_cache_clear`: clear FOB cache
- `fob_query <query_type>`: query FOB targets (query types: ENEMY, PICKUP, PICKUP_HIGH, NUCLEAR, etc.)
- `fob_open_wormhole <target_steam_id>`: open a wormhole to a target FOB (sends CMD_OPEN_WORMHOLE directly to Konami servers, automatically detects both your player_id and target's player_id from cache)
- `fob_get_target_detail <target_steam_id>`: manually fetch FOB target details by steam_id (uses cached player info, sends CMD_GET_FOB_TARGET_DETAIL)
- `fob_convert_list_type <from> <to>`: convert the type field in CMD_GET_FOB_TARGET_LIST requests (e.g. `fob_convert_list_type PICKUP FOLLOW`)
- `fob_status`: show current FOB status and configuration

> **Blockade Bypass**: The `fob_open_wormhole` command can be used to bypass FOB blockade by sending `CMD_OPEN_WORMHOLE` directly to the server. This creates a wormhole similar to the retaliation wormhole mechanism.
> 
> **Prerequisites for `fob_open_wormhole`**:
> 1. Session key from `CMD_REQAUTH_HTTPS` (auto-captured when logging in)
> 2. Your `player_id` in cache (auto-populated when accessing FOB menu)
> 3. Target `player_id` in cache (auto-populated when browsing FOB lists)
> 
> **Usage**:
> 1. Login to the game and access the FOB menu
> 2. Browse FOB lists to populate the cache with target players
> 3. Use `fob_open_wormhole <target_steam_id>` in the console
> 4. Return to FOB menu and re-select the target

**FOB List Types:**

| Type | ID | Description |
| --- | --- | --- |
| TRIAL | 1 | Training / Visit Destination |
| PICKUP | 2 | Infiltration Targets (PFs of equal grade) |
| PICKUP_HIGH | 3 | Infiltration Targets (High-Ranking PFs) |
| ENEMY | 4 | Retaliation Targets |
| EVENT | 5 | Events |
| NUCLEAR | 6 | Nuclear-equipped Targets |
| FOLLOW | 7 | Supporting |
| FOLLOWER | 8 | Supporters |
| DEPLOYED | 9 | FOB Unit Deployed List |
| INJURY | 10 | Intruder |
| EMERGENCY | 11 | Emergency |
| FR_ENEMY | 12 | Indirect Retaliation Targets |

### Variable list
similar to cod dvars, can be set through the console or through the config files `%localappdata%/tpp-mod/config/`
(type their name in the console for a description)

**Network Variables**
- `net_custom_server`: custom server url
- `net_channel`: steam networking channel (0-65535)
- `net_udp`: use udp sockets instead of steam networking (0/1)
- `net_port`: udp socket port (0-65535, default: 5377)
- `net_server_logging`: enable server logging (0/1)
- `net_server_heartbeat`: backend server heartbeat interval

**FOB Variables**

**Match Variables**
- `match_enable_tweaks`: enable match settings tweaks (0/1)
- `match_min_players`: match minimum players override (0-16, default: 2)
- `match_max_players`: match maximum players override (0-16, default: 16)
- `match_briefing_time`: match briefing time override in seconds (0-600, default: 60)

**Performance Variables**
- `com_worker_count`: worker thread count (default: 4)
- `com_unlock_fps`: unlock fps (0/1)
- `com_max_fps`: max fps cap (TPP: 0-1000, MGO: 30-60)
- `sensitivity`: mouse sensitivity scale (0-10, default: 1.0)
- `camera_fov_scale`: camera FOV scale (0.1-5, default: 1.0)
- `camera_first_person_fov_scale`: first person camera FOV scale (0.1-5, default: 1.0)
- `player_ramble_speed_scale`: player ramble speed scale (TPP only)
- `player_ramble_speed_patch`: enable player ramble speed patch (TPP only)
- `ui_skip_intro`: skip intro splashscreens (TPP only)

**UI Variables**
- `ui_draw_fps`: draw fps counter (0/1)
- `ui_draw_ping`: draw ping counter (0/1)

**Console Variables**
- `con_input_box_color`: console input box color
- `con_input_hint_box_color`: console input hint box color
- `con_output_bar_color`: console output bar color
- `con_output_slider_color`: console output slider color
- `con_output_window_color`: console output window color
- `con_input_dvar_match_color`: console input dvar match color
- `con_input_dvar_value_color`: console input dvar value color
- `con_input_dvar_inactive_value_color`: console input dvar inactive value color
- `con_input_cmd_match_color`: console input command match color
- `console_log`: save console log to file (0/1, saved to tpp-mod/log/console.log)

**Chat Variables**
- `chat_enable`: enable mgo text chat (0/1, default: 1)
- `chat_time`: chat message duration in milliseconds (0-60000, default: 10000)
- `chat_input_bg`: chat input background color
- `chat_output_bg`: chat output background color
- `chat_slider_color`: chat slider color
- `chat_input_pulse`: chat input pulse effect (0/1)
- `chat_offset`: chat window offset (x, y)
- `chat_height`: chat window height
- `chat_width`: chat window width (0-1, default: 0.35)
- `chat_scale`: chat window scale (0-1, default: 1.0)
- `chat_direction`: chat message direction (0=top-to-bottom, 1=bottom-to-top)
- `chat_muted_players`: muted player list

**Other Variables**
- `discord_enable`: enable discord rpc (0/1)
- `dsx_enable`: enable DSX integration (0/1)
- `name`: player name
- `staff_cheat`: max staff ranks and skills (cheat only, requires -var-cheat)
- `lua_logging`: enable lua logging (0=disabled, 1=print scripts, 2=enable log prints)
- `lua_dump`: dump lua scripts (0/1)

## Launch parameters

- `-mode <mode>`: overriding version detection (modes: 'tppeng', 'mgoeng', 'tppjpn', 'mgojpn')
- `-dedicated`: start mgo headless as dedicated server
- `-var-cheat`: allow to modify cheat only vars
- `-dump-hashes`: stores computed hashes, can be dumped with `dumpstrings` command
