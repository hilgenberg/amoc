#ifndef INTERFACE_ELEMENTS_H
#define INTERFACE_ELEMENTS_H

#include <ncurses.h>
#include <wctype.h>
#include <wchar.h>

#include "../lists.h"
#include "../files.h"
#include "keys.h"

/* Interface's menus */
enum iface_menu
{
	IFACE_MENU_PLIST,
	IFACE_MENU_DIR
};

typedef void t_user_reply_callback (const char *reply, void *data);

enum entry_type
{
	ENTRY_SEARCH,
	ENTRY_PLIST_SAVE,
	ENTRY_GO_DIR,
	ENTRY_GO_URL,
	ENTRY_ADD_URL,
	ENTRY_PLIST_OVERWRITE,
	ENTRY_USER_QUERY
};

enum iface_key_type
{
	IFACE_KEY_CHAR,	    /* Regular char */
	IFACE_KEY_FUNCTION  /* Function key (arrow, F12, etc.) */
};

struct iface_key
{
	/* Type of the key */
	iface_key_type type;

	union {
		wchar_t ucs;        /* IFACE_KEY_CHAR */
		int func;           /* IFACE_KEY_FUNCTION */
	} key;
};

void windows_init ();
void windows_reset ();
void windows_end ();
void iface_set_option_state (const char *name, const bool value);
void iface_set_mixer_name (const char *name);
void iface_set_status (const char *msg);
void iface_set_dir_content (const enum iface_menu iface_menu,
		const struct plist *files,
		const stringlist *dirs,
		const stringlist *playlists);
void iface_update_dir_content (const enum iface_menu iface_menu,
		const struct plist *files,
		const stringlist *dirs,
		const stringlist *playlists);
void iface_set_curr_item_title (const char *title);
void iface_get_key (struct iface_key *k);
int iface_key_is_resize (const struct iface_key *k);
void iface_menu_key (const enum key_cmd cmd);
enum file_type iface_curritem_get_type ();
int iface_in_dir_menu ();
int iface_in_plist_menu ();
char *iface_get_curr_file ();
void iface_update_item (const enum iface_menu menu, const struct plist *plist,
		const int n);
void iface_set_curr_time (const int time);
void iface_set_total_time (const int time);
void iface_set_state (const int state);
void iface_set_bitrate (const int bitrate);
void iface_set_rate (const int rate);
void iface_set_channels (const int channels);
void iface_set_played_file (const char *file);
void iface_set_played_file_title (const char *title);
void iface_set_mixer_value (const int value);
void iface_set_files_in_queue (const int num);
void iface_tick ();
void iface_switch_to_plist ();
void iface_switch_to_dir ();
void iface_add_to_plist (const struct plist *plist, const int num);
void iface_error (const char *msg);
void iface_resize ();
void iface_refresh ();
void iface_update_show_time ();
void iface_update_show_format ();
void iface_clear_plist ();
void iface_del_plist_item (const char *file);
enum entry_type iface_get_entry_type ();
int iface_in_entry ();
void iface_make_entry (const enum entry_type type);
void iface_entry_handle_key (const struct iface_key *k);
void iface_entry_set_text (const char *text);
char *iface_entry_get_text ();
void iface_entry_history_add ();
void iface_entry_disable ();
void iface_entry_set_file (const char *file);
char *iface_entry_get_file ();
void iface_message (const char *msg);
void iface_disable_message ();
void iface_user_query (const char *msg, const char *prompt, t_user_reply_callback *callback, void *data);
void iface_user_reply (const char *reply);
void iface_user_history_add (const char *text);
void iface_plist_set_total_time (const int time, const int for_all_files);
void iface_set_title (const enum iface_menu menu, const char *title);
void iface_select_file (const char *file);
void iface_toggle_layout ();
void iface_toggle_percent ();
void iface_swap_plist_items (const char *file1, const char *file2);
void iface_make_visible (const enum iface_menu menu, const char *file);
void iface_add_file (const char *file, const char *title,
		const enum file_type type);
void iface_temporary_exit ();
void iface_restore ();
void iface_update_queue_positions (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list,
		const char *deleted_file);
void iface_clear_queue_positions (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list);
void iface_update_queue_position_last (const struct plist *queue,
		struct plist *playlist, struct plist *dir_list);
void iface_update_attrs ();

#endif