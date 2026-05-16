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

- Download dinput8.dll from the release page and copy it to the game folder.  

Configuration files are stored in `%localappdata%\tpp-mod`

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
- **Dispatch intercept system (100% dispatch success rate)**

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
- `fob_connect <steam_id>`: connect to a specific FOB target by Steam ID (must be in FOB menu with list loaded)
- `fob_add_target <steam_id> <player_id> <name>`: add a custom FOB target
- `fob_remove_target <steam_id>`: remove a custom FOB target
- `fob_clear_targets`: clear all custom FOB targets
- `fob_target_list`: list all custom FOB targets
- `fob_cache_list`: list all cached FOB targets
- `fob_cache_clear`: clear FOB cache
- `fob_query <query_type>`: query FOB targets (query types: ENEMY, PICKUP, PICKUP_HIGH, NUCLEAR, etc.)
- `fob_follow <steam_id>`: follow a FOB target
- `fob_unfollow <steam_id>`: unfollow a FOB target

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
- `fob_security_challenge_mode`: security challenge mode (0 = konami, 1 = steam lobbies)
- `fob_override_list_type`: FOB list type to override (0=disabled, 1=TRIAL, 2=PICKUP, 3=PICKUP_HIGH, 4=ENEMY, 5=EVENT, 6=NUCLEAR, 7=FOLLOW, 8=FOLLOWER, 9=DEPLOYED, 10=INJURY, 11=EMERGENCY, 12=FR_ENEMY)
- `fob_override_list_mode`: FOB list override mode (0=replace, 1=append)
- `dispatch_intercept_mode`: Dispatch intercept mode (0 = disabled, 1 = enabled, 100% success rate)
- `dispatch_success_rate`: Dispatch fake success rate (0-100, default 100)

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
