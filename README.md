# tpp-mod

Improvements and fixes for Metal Gear Solid V: The Phantom Pain

based on [alicealys/tpp-mod](https://github.com/alicealys/tpp-mod)

discord server
https://discord.gg/hYfW9MEEGF

## Installation

tpp-mod only supports original steam releases (latest).  
Any kind of modified or pirated version is **NOT** supported.   

For older builds go here:
- [23 March 2021](https://github.com/alicealys/tpp-mod/tree/day1820)
- [6 June 2026](https://github.com/alicealys/tpp-mod/tree/day3800)

| version | sha1 | support |
| --- | --- | --- |
| mgsvtpp.exe (eng, 1.0.15.4) | 4F643576C6F6ECF6C67A9AEF4D4F02285ADA43E3 | ✔️ |
| mgsvmgo.exe (eng, 1.1.2.9) | 1582B87C25CC989FE5457B9F1FD90ACACA9BCD89 | ✔️ |
| mgsvtpp.exe (jpn, 1.0.15.4) | 72F63B2E3364A36C2FA0BE360AF17A787FCC14B0 | ❌ |
| mgsvmgo.exe (jpn, 1.1.2.9) | 4F65C0DB3A136325D5C358D01680C4D7DE61E566 | ❌ |

- Download [dinput8.dll](https://github.com/MXYLR/tpp-mod/releases/latest/download/dinput8.dll) and copy it to the game folder.
- Download [fob-gui.exe](https://github.com/MXYLR/tpp-mod/releases/latest/download/fob-gui.exe) and place it next to the game executable (optional, used by the FOB GUI).

Configuration files are stored in the game folder itself (`config_tpp.cfg`, `config_mgo.cfg`, `keys_tpp.cfg`, `fob-gui.cfg`).

## Features

| <img src="assets/github/1.png?raw=true" /> |
|:-:|
| in game text chat & ping/fps counter |

| <img src="assets/github/2.png?raw=true" /> |
|:-:|
| in game console |

### Font replacement

The game's own fonts can be replaced at runtime with any ttf/otf font, no `.ffnt` repacking
needed. Point `r_font_file` at a font file or a folder and restart the game:

```
set r_font_file "C:/Windows/Fonts/SourceHanSansCN-Regular.otf"
set r_font_chinese "1"
```

- every glyph is drawn at the size the font itself draws it at, so a kanji, a kana and the
  latin beside it come out in the proportion the font was designed with, and the text keeps
  the size the game's own fonts had
- the characters the game's japanese fonts do not have are covered by the common chinese set
  (gb2312 level 1, 3755 characters), which is what makes a translation readable at all
- `r_font_scale` changes the size of the whole font, `r_font_kanji_scale` only its
  kanji/kana/chinese half, `r_font_latin_scale` only its latin half
- `r_font_ranges` and `r_font_extra` add characters beyond the ones the game already uses
- takes effect on restart

### Security

Various crash/DoS exploits and other vulnerabilities have been patched. 

### MGO3 Dedicated Server

To start a MGO3 dedicated server run the game with the following command:

```
./mgsvmgo.exe -dedicated +exec server.cfg
```

The config file can be downloaded [here](src/client/resources/server.cfg).  
Copy it to the game folder and edit it to your preference.  

### Misc
- discord rich presence
- FOB GUI (fob-gui.exe): manage FOBs and use wormhole communication via named pipes
- unlock fps and custom fps capping
- fps and ping counter
- mgo text chat
- in game console
- ttf/otf font replacement for the game's own fonts (see: [Font replacement](#font-replacement))
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

### Command list
Can be executed from the console or in game console 

- `bind <key> <command>`: bind a key to a command
- `remap <key from> <key to>`: remap a key to another
- `unbind <key>`: remove custom bind from key
- `unbindall`: remove all custom binds
- `exec <cfg name>`: execute a config file 
- `alias <name> <command>`: create a command alias 
- `clear`: clear the ingame console
- `toggleconsole`: toggle the ingame console
- `equip <slot type> <index>`: equip a slot (slot types: 'primary1', 'primary2', 'secondary', 'support', 'item', 'stole', hand')
- `equiptoggle <slot type> <index 1> <index 2>`: toggle between 2 equip slots
- `equipnext <slot type>`: equip the next slot
- `equipprev <slot type>`: equip the previous slot
- `set <var name> <value>`: set a var
- `reset <var name>`: reset a var
- `var_list`: show the list of vars
- `quit`: quit the game
- `startsound <id>`: start playing a sound
- `stopsound <id>`: stop playing the sound
- `wait <frame count>`: wait before executing the next command
- `startmgo`: start mgo
- `starttpp`: start tpp
- `script_var <varname>`: view lua script var
- `script_exec <code>`: execute lua code
- `script_load <path>`: load a lua script
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
- `matchstart`: start a match
- `matchrotate`: rotate the match to the next slot
- `matchset <name> <value>`: set a match setting
- `matchsetrule <name> <value>`: set a match rule
- `matchsetslot <slot> <name> <value>`: set a match' slot rule
- `kick <name|steamid|index>`: kick a player from the lobby
- `clearchat`: clear the chat
- `chatall`: open the all chat
- `chatteam`: open the team chat
- `say <message>`: send a message to all chat
- `say_team <message>`: send a message to team chat
- `mute <name|steamid|index>`: mute a player
- `unmute <name|steamid|index>`: unmute a player
- `mutelist`: show mutes players
- `clearmutes`: unmute all players

### Variable list
similar to cod dvars, can be set through the console or through the config files in the game folder `config/`
(type their name in the console for a description)

- `net_custom_server`
- `net_proxy_url`
- `net_channel`
- `net_server_logging`
- `net_server_heartbeat`
- `whitelist_enable`
- `discord_enable`
- `dsx_enable`
- `ui_mb_color`
- `ui_color_tweaks`
- `ui_disable_noise`
- `ui_disable_dot_pattern`
- `ui_draw_fps`
- `ui_draw_ping`
- `ui_draw_mod`
- `ui_skip_intro`
- `con_input_box_color`
- `con_input_hint_box_color`
- `con_output_bar_color`
- `con_output_slider_color`
- `con_output_window_color`
- `con_input_dvar_match_color`
- `con_input_dvar_value_color`
- `con_input_dvar_inactive_value_color`
- `con_input_cmd_match_color`
- `r_custom_text_rendering`
- `r_draw2d`
- `r_draw_priority_test`
- `r_fog`
- `r_fxaa`
- `r_font_file`
- `r_font_name`
- `r_font_size`
- `r_font_scale`
- `r_font_kanji_scale`
- `r_font_latin_scale`
- `r_font_x_offset`
- `r_font_y_offset`
- `r_font_ranges`
- `r_font_extra`
- `r_font_chinese`
- `r_extra_font_1`
- `r_extra_font_2`
- `com_worker_count`
- `com_unlock_fps`
- `com_max_fps`
- `sensitivity`
- `sensitivity_fps_patch`
- `menu_sensitivity`
- `camera_fovscale`
- `camera_fp_fovscale`
- `camera_fp_preserve`
- `player_ramble_speed_scale`
- `player_ramble_speed_patch`
- `name`
- `lua_logging`
- `lua_dump`
- `fs_mod`
- `sv_base_url`
- `mapname`
- `match_enable_tweaks`
- `match_min_players`
- `match_max_players`
- `match_briefing_time`
- `match_password`
- `match_restricted`
- `match_host_comment`
- `fob_security_challenge_mode`
- `fob_security_challenge_min_level`
- `chat_enable`
- `chat_time`
- `chat_input_bg`
- `chat_output_bg`
- `chat_slider_color`
- `chat_input_pulse`
- `chat_offset`
- `chat_height`
- `chat_width`
- `chat_scale`
- `chat_direction`
- `chat_muted_players`
- `net_udp`
- `net_port`
- `cheat_enabled`
- `cheat_disable_reporting`
- `cheat_develop_limit`
- `cheat_no_deployment_cost`
- `cheat_unlockall_gear`
- `cheat_unlockall_server_items`

## Launch parameters

- `-mode <mode>`: overriding version detection (modes: 'tppeng', 'mgoeng', 'tppjpn', 'mgojpn')
- `-dedicated`: start mgo headless as dedicated server
- `-var-cheat`: allow to modify cheat only vars
- `-dump-hashes`: stores computed hashes, can be dumped with `dumpstrings` command
- `-noconsole`: disable external console
