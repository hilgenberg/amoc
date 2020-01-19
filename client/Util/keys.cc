/*
 * MOC - music on console
 * Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include "keys.h"
#include <ncurses.h>

static std::map<int, key_cmd> cmds[3]; // index is CON_XXX, key is keycode, value is KEY_CMD_XXX
static std::map<key_cmd, str> keys; // KEY_CMD_XXX --> key_name(key)

key_cmd get_key_cmd (key_context context, wchar_t c, int f)
{
	auto &m = cmds[(int)context];
	auto it = m.find(c ? (int)c : f);
	return it != m.end() ? it->second : KEY_CMD_WRONG;
}
str hotkey(key_cmd cmd)
{
	auto it = keys.find(cmd);
	return it != keys.end() ? it->second : str();
}

//--- rest is init ------------------------------------------
#define CTRL_KEY_CODE	0x1F

/* ^c version of c */
#ifndef CTRL
# define CTRL(c) ((c) & CTRL_KEY_CODE)
#endif

static struct special_keys
{
	const char *name;
	int key;
} special_keys[] = {
	{ "DOWN",		KEY_DOWN },
	{ "UP",			KEY_UP },
	{ "LEFT",		KEY_LEFT },
	{ "RIGHT",		KEY_RIGHT },
	{ "HOME",		KEY_HOME },
	{ "BACKSPACE",		KEY_BACKSPACE },
	{ "DEL",		KEY_DC },
	{ "INS",		KEY_IC },
	{ "ENTER",		'\n' },
	{ "PAGE_UP",		KEY_PPAGE },
	{ "PAGE_DOWN",		KEY_NPAGE },
	{ "TAB",		'\t' },
	{ "END",		KEY_END },
	{ "KEYPAD_CENTER",	KEY_B2 },
	{ "SPACE",		' ' },
	{ "ESCAPE",		KEY_ESCAPE },
	{ "F1",			KEY_F(1) },
	{ "F2",			KEY_F(2) },
	{ "F3",			KEY_F(3) },
	{ "F4",			KEY_F(4) },
	{ "F5",			KEY_F(5) },
	{ "F6",			KEY_F(6) },
	{ "F7",			KEY_F(7) },
	{ "F8",			KEY_F(8) },
	{ "F9",			KEY_F(9) },
	{ "F10",		KEY_F(10) },
	{ "F11",		KEY_F(11) },
	{ "F12",		KEY_F(12) },
	{ NULL, 0 }
};

/* Get a nice key name. */
static str key_name(int key)
{
	/* Search for special keys */
	for (auto *k = special_keys; k->name; ++k)
		if (k->key == key) return k->name;

	char key_str[4];
	/* CTRL combination */
	if (!(key & ~CTRL_KEY_CODE)) {
		key_str[0] = '^';
		key_str[1] = key + 0x60;
		key_str[2] = 0;

		return key_str;
	}

	/* Meta keys */
	if (key & META_KEY_FLAG) {
		strcpy (key_str, "M-");
		key_str[2] = key & ~META_KEY_FLAG;
		key_str[3] = 0;

		return key_str;
	}

	/* Normal key */
	key_str[0] = key;
	key_str[1] = 0;

	return key_str;
}

static void keymap_parse_error (const int line, const str &msg)
{
	error ("Parse error in the keymap file line %d: %s\n", line, msg.c_str());
}

/* Return a key for the symbolic key name (^c, M-F, etc.).
 * Return -1 on error. */
static int parse_key (const char *sym)
{
	if (strlen(sym) == 1) return sym[0];
	if (sym[0] == '^') return strlen(sym) == 2 ? CTRL(sym[1]) : -1;
	if (!strncasecmp(sym, "M-", 2))
		return strlen(sym) == 3 ? sym[2] | META_KEY_FLAG : -1;
	for (auto *k = special_keys; k->name; ++k)
		if (!strcasecmp(k->name, sym)) return k->key;
	return -1;
}

struct command
{
	command(key_cmd cmd, int k, key_context ctx) : cmd(cmd), ctx(ctx), key(k) {}
	key_context ctx;
	key_cmd     cmd;
	int         key;
};

/* Load key map. Set default keys if necessary. */
void keys_init ()
{
	std::map<str, command> init_map;
	#define ADD(cmd, name, k, ctx) assert(!init_map.count(name)); init_map.insert(std::make_pair(name, command(cmd, k, ctx)))

	// CON_MENU commands
	#define CMD(cmd, name, k) ADD(cmd, name, k, CON_MENU)
	CMD(KEY_CMD_MENU_EXEC,		"menu_execute",	'\n'); // Execute current menu item
	CMD(KEY_CMD_MENU_EXEC_NOCLOSE,	"menu_execute_without_closing",	' '); // Execute current menu item without closing the menu
	CMD(KEY_CMD_MENU_DOWN,		"menu_down",	KEY_DOWN); // Move down in the menu
	CMD(KEY_CMD_MENU_UP,		"menu_up",	KEY_UP); // Move up in the menu
	CMD(KEY_CMD_MENU_LEFT,		"menu_left",	KEY_LEFT); // Move left in the menu
	CMD(KEY_CMD_MENU_RIGHT,		"menu_right",	KEY_RIGHT); // Move right in the menu
	CMD(KEY_CMD_MENU_NPAGE,		"menu_page_down",	KEY_NPAGE); // Move one page down
	CMD(KEY_CMD_MENU_PPAGE,		"menu_page_up",	KEY_PPAGE); // Move one page up
	CMD(KEY_CMD_MENU_FIRST,		"menu_first_item",	KEY_HOME); // Move to the first item in the menu
	CMD(KEY_CMD_MENU_LAST,		"menu_last_item",	KEY_END); // Move to the last item in the menu
	CMD(KEY_CMD_MENU,		"menu_hide",	KEY_ESCAPE); // Show or hide the menu
	#undef CMD

	// CON_ENTRY commands
	#define CMD(cmd, name, k) ADD(cmd, name, k, CON_ENTRY)
	CMD(KEY_CMD_CANCEL,		"cancel",	KEY_ESCAPE); // Exit from an entry
  	CMD(KEY_CMD_HISTORY_UP,		"history_up",	KEY_UP); // Go to the previous entry in the history (entry)
 	CMD(KEY_CMD_HISTORY_DOWN,	"history_down",	KEY_DOWN); // Go to the next entry in the history (entry)
 	CMD(KEY_CMD_DELETE_START,	"delete_to_start",	CTRL('u')); // Delete to start of line (entry)
 	CMD(KEY_CMD_DELETE_END,		"delete_to_end",	CTRL('k')); // Delete to end of line (entry)
	#undef CMD

	// CON_PANEL commands
	#define CMD(cmd, name, k) ADD(cmd, name, k, CON_PANEL)
	CMD(KEY_CMD_QUIT_CLIENT,	"quit_client",	'q'); // Detach MOC from the server
	CMD(KEY_CMD_GO,			"go",	'\n'); // Start playing at this file or go to this directory
	CMD(KEY_CMD_MENU_DOWN,		"panel_down",	KEY_DOWN); // Move down in the menu
	CMD(KEY_CMD_MENU_UP,		"panel_up",	KEY_UP); // Move up in the menu
	CMD(KEY_CMD_MENU_LEFT,		"panel_left",	KEY_LEFT); // Move left in the menu
	CMD(KEY_CMD_MENU_RIGHT,		"panel_right",	KEY_RIGHT); // Move right in the menu
	CMD(KEY_CMD_MENU_EXTEND_DOWN,	"panel_extend_down",	336); // Extend selection downwards
	CMD(KEY_CMD_MENU_EXTEND_UP,	"panel_extend_up",	337); // Extend selection upwards
	CMD(KEY_CMD_MENU_NPAGE,		"panel_page_down",	KEY_NPAGE); // Move one page down
	CMD(KEY_CMD_MENU_PPAGE,		"panel_page_up",	KEY_PPAGE); // Move one page up
	CMD(KEY_CMD_MENU_FIRST,		"panel_first_item",	KEY_HOME); // Move to the first item in the menu
	CMD(KEY_CMD_MENU_LAST,		"panel_last_item",	KEY_END); // Move to the last item in the menu
	CMD(KEY_CMD_QUIT,		"quit",	'Q'); // Quit
	CMD(KEY_CMD_STOP,		"stop",	's'); // Stop
	CMD(KEY_CMD_NEXT,		"next",	'n'); // Play next file
	CMD(KEY_CMD_PREVIOUS,		"previous",'b'); // Play previous file
	CMD(KEY_CMD_PAUSE,		"pause", ' '); // Pause
	
	CMD(KEY_CMD_TOGGLE_READ_TAGS,	"toggle_read_tags",	'f'); // Toggle ReadTags option
	CMD(KEY_CMD_TAG_ARTIST,		"tag_artist",	KEY_F(1));
	CMD(KEY_CMD_TAG_ALBUM,		"tag_album",	KEY_F(2));
	CMD(KEY_CMD_TAG_TITLE,		"tag_title",	KEY_F(3));
	CMD(KEY_CMD_WRITE_TAGS,		"tag_flush",	CTRL('s'));
	CMD(KEY_CMD_TAG_ADD_NUMBERS,	"tag_add_track_numbers", 0);
	CMD(KEY_CMD_TAG_DEL_NUMBERS,	"tag_del_track_numbers", 0);

	CMD(KEY_CMD_TOGGLE_SHUFFLE,	"toggle_shuffle",	'S'); // Toggle Shuffle
	CMD(KEY_CMD_TOGGLE_REPEAT,	"toggle_repeat",	'R'); // Toggle Repeat
	CMD(KEY_CMD_TOGGLE_AUTO_NEXT,	"toggle_auto_next",	'X'); // Toggle AutoNext
	CMD(KEY_CMD_TOGGLE_MENU,	"toggle_menu",	'\t'); // Switch between playlist and file list
	CMD(KEY_CMD_TOGGLE_LAYOUT,	"toggle_layout",	'l'); // Switch between layouts
	CMD(KEY_CMD_PLIST_ADD,		"add",	'a'); // Add a file/directory to the playlist
	CMD(KEY_CMD_PLIST_INS,		"insert",	'i'); // Insert a file/directory after the current song
	CMD(KEY_CMD_PLIST_CLEAR,	"clear_playlist",	'C'); // Clear the playlist
	CMD(KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES,	"remove_dead_entries",	'Y'); // Remove playlist entries for non-existent files
	CMD(KEY_CMD_MIXER_DEC_1,	"volume_down_1",	'<'); // Decrease volume by 1%
	CMD(KEY_CMD_MIXER_INC_1,	"volume_up_1",	'>'); // Increase volume by 1%
	CMD(KEY_CMD_MIXER_DEC_5,	"volume_down_5",	','); // Decrease volume by 5%
	CMD(KEY_CMD_MIXER_INC_5,	"volume_up_5",	'.'); // Increase volume by 5%
	CMD(KEY_CMD_SEEK_FORWARD,	"seek_forward",	KEY_RIGHT); // Seek forward by n-s
	CMD(KEY_CMD_SEEK_BACKWARD,	"seek_backward",KEY_LEFT); // Seek backward by n-s
	CMD(KEY_CMD_HIDE_MESSAGE,	"hide_message",	'M'); // Hide error/informative message
	CMD(KEY_CMD_REFRESH,	"refresh",	CTRL('l')); // Refresh the screen
	CMD(KEY_CMD_RELOAD,	"reload",	CTRL('r')); // Reread directory content
	CMD(KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES,	"toggle_hidden_files",	'H'); // Toggle ShowHiddenFiles option
	CMD(KEY_CMD_GO_MUSIC_DIR,	"go_to_music_directory",	'm'); // Go to the music directory (requires config option)
	CMD(KEY_CMD_PLIST_DEL,	"delete_from_playlist",	'd'); // Delete an item from the playlist
	CMD(KEY_CMD_MENU_SEARCH,	"search_menu",	'/'); // Search the menu
	CMD(KEY_CMD_PLIST_SAVE,	"save_playlist",	'V'); // Save the playlist
	CMD(KEY_CMD_GO_URL,	"go_url",	'o'); // Play from the URL
	CMD(KEY_CMD_GO_TO_PLAYING_FILE,	"go_to_playing_file",	'G'); // Go to the currently playing file's directory
	CMD(KEY_CMD_GO_DIR,	"go_to_a_directory",	0); // Go to a directory
	CMD(KEY_CMD_GO_DIR_UP,	"go_up",	0); // Go to '..'
	
	CMD(KEY_CMD_SEEK_FORWARD_5,	"seek_forward_fast",	']'); // Silent seek forward by 5s
	CMD(KEY_CMD_SEEK_BACKWARD_5,	"seek_backward_fast",	'['); // Silent seek backward by 5s
	CMD(KEY_CMD_VOLUME_10,	"volume_10",	'1' | META_KEY_FLAG); // Set volume to 10%
	CMD(KEY_CMD_VOLUME_20,	"volume_20",	'2' | META_KEY_FLAG); // Set volume to 20%
	CMD(KEY_CMD_VOLUME_30,	"volume_30",	'3' | META_KEY_FLAG); // Set volume to 30%
	CMD(KEY_CMD_VOLUME_40,	"volume_40",	'4' | META_KEY_FLAG); // Set volume to 40%
	CMD(KEY_CMD_VOLUME_50,	"volume_50",	'5' | META_KEY_FLAG); // Set volume to 50%
	CMD(KEY_CMD_VOLUME_60,	"volume_60",	'6' | META_KEY_FLAG); // Set volume to 60%
	CMD(KEY_CMD_VOLUME_70,	"volume_70",	'7' | META_KEY_FLAG); // Set volume to 70%
	CMD(KEY_CMD_VOLUME_80,	"volume_80",	'8' | META_KEY_FLAG); // Set volume to 80%
	CMD(KEY_CMD_VOLUME_90,	"volume_90",	'9' | META_KEY_FLAG); // Set volume to 90%

	CMD(KEY_CMD_RATE_0,	"rate_0",	'0'); // Set rating to 0 stars
	CMD(KEY_CMD_RATE_1,	"rate_1",	'1'); // Set rating to 1 star
	CMD(KEY_CMD_RATE_2,	"rate_2",	'2'); // Set rating to 2 stars
	CMD(KEY_CMD_RATE_3,	"rate_3",	'3'); // Set rating to 3 stars
	CMD(KEY_CMD_RATE_4,	"rate_4",	'4'); // Set rating to 4 stars
	CMD(KEY_CMD_RATE_5,	"rate_5",	'5'); // Set rating to 5 stars

	CMD(KEY_CMD_SEEK_0,	"seek_0",	0); // Seek to start of song
	CMD(KEY_CMD_SEEK_1,	"seek_1",	0); // Seek to 10% of song
	CMD(KEY_CMD_SEEK_2,	"seek_2",	0); // Seek to 20% of song
	CMD(KEY_CMD_SEEK_3,	"seek_3",	0); // Seek to 30% of song
	CMD(KEY_CMD_SEEK_4,	"seek_4",	0); // Seek to 40% of song
	CMD(KEY_CMD_SEEK_5,	"seek_5",	0); // Seek to 50% of song
	CMD(KEY_CMD_SEEK_6,	"seek_6",	0); // Seek to 60% of song
	CMD(KEY_CMD_SEEK_7,	"seek_7",	0); // Seek to 70% of song
	CMD(KEY_CMD_SEEK_8,	"seek_8",	0); // Seek to 80% of song
	CMD(KEY_CMD_SEEK_9,	"seek_9",	0); // Seek to 90% of song
 	CMD(KEY_CMD_TOGGLE_MIXER,	"toggle_mixer",	'x'); // Toggles the mixer channel
 	CMD(KEY_CMD_TOGGLE_SOFTMIXER,	"toggle_softmixer",	'w'); // Toggles the software-mixer
 	CMD(KEY_CMD_TOGGLE_EQUALIZER,	"toggle_equalizer",	'E'); // Toggles the equalizer
 	CMD(KEY_CMD_EQUALIZER_REFRESH,	"equalizer_refresh",	'e'); // Reload EQ-presets
 	CMD(KEY_CMD_EQUALIZER_PREV,	"equalizer_prev",	'K'); // Select previous equalizer-preset
 	CMD(KEY_CMD_EQUALIZER_NEXT,	"equalizer_next",	'k'); // Select next equalizer-preset
 	CMD(KEY_CMD_TOGGLE_MAKE_MONO,	"toggle_make_mono",	'J'); // Toggle mono-mixing
 	CMD(KEY_CMD_PLIST_MOVE_UP,	"plist_move_up",	'u'); // Move playlist item up
 	CMD(KEY_CMD_PLIST_MOVE_DOWN,	"plist_move_down",	'j'); // Move playlist item down
 	CMD(KEY_CMD_ADD_STREAM,	"plist_add_stream",	'U'); // Add a URL to the playlist using entry
	CMD(KEY_CMD_MENU,	"menu_show",	KEY_F(9)); // Show or hide the menu

 	CMD(KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS,	"playlist_full_paths",	'P'); // Toggle displaying full paths in the playlist
	#undef CMD

	#undef ADD

	str file_name = options::config_file_path("keymap");
	FILE *file = fopen(file_name.c_str(), "r");
	char *line;
	int line_num = 0;
	if (!file)
	{
		logit ("Can't open keymap file: %s - %s", file_name.c_str(), xstrerror (errno));
	}
	else
	{
		/* Read lines in format:
		* COMMAND = KEY [KEY ...]
		* Blank lines and beginning with # are ignored, see example_keymap. */
		while ((line = read_line(file)))
		{
			++line_num;

			char *command;
			if (line[0] == '#' || !(command = strtok(line, " \t"))) {
				/* empty line or a comment */
				free (line);
				continue;
			}

			auto it = init_map.find(command);
			if (it == init_map.end())
			{
				keymap_parse_error (line_num, format("unknown command: %s", command));
				free(line);
				continue;
			}
			auto &cmd = it->second;

			char *tmp = strtok(NULL, " \t");
			if (!tmp || strcmp(tmp, "="))
			{
				keymap_parse_error (line_num, "expected '='");
				free(line);
				continue;
			}
			
			// clear default key
			cmd.key = 0;

			char *key;
			while ((key = strtok(NULL, " \t")))
			{
				int k = parse_key(key);
				if (k == -1)
				{
					keymap_parse_error (line_num, format("bad key sequence: %s", key));
					continue;
				}

				cmds[(int)cmd.ctx][k] = cmd.cmd;
				if (!keys.count(cmd.cmd)) keys[cmd.cmd] = key_name(k);
			}
			free (line);
		}
		fclose (file);
	}

	for (auto &it : init_map)
	{
		auto &cmd = it.second;

		if (!cmd.key) continue;
		auto &m = cmds[(int)cmd.ctx];
		if (!m.count(cmd.key))
		{
			m[cmd.key] = cmd.cmd;
			if (!keys.count(cmd.cmd)) keys[cmd.cmd] = key_name(cmd.key);
		}
	}
};
