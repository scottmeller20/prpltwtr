/**
 * TODO: legal stuff
 *
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include <glib/gstdio.h>

#include "twitter.h"

#define TWITTER_PROTOCOL_ID "prpl-twitter"
static PurplePlugin *_twitter_protocol = NULL;

#define TWITTER_STATUS_ONLINE   "online"
#define TWITTER_STATUS_OFFLINE   "offline"

#define MAX_TWEET_LENGTH 140

typedef struct _TwitterUserData TwitterUserData;
typedef struct _TwitterStatusData TwitterStatusData;

struct _TwitterUserData
{
	PurpleAccount *account;
	long long id;
	char *name;
	char *screen_name;
	char *profile_image_url;
	char *description;
};

struct _TwitterStatusData
{
	char *text;
	long long id;
	long long in_reply_to_status_id;
	char *in_reply_to_screen_name;
	time_t created_at;
};

typedef enum
{
	TWITTER_CHAT_SEARCH = 0,
	TWITTER_CHAT_TIMELINE = 1
} TwitterChatType;

typedef struct
{
	TwitterStatusData *status;
	TwitterUserData *user;
} TwitterBuddyData;

typedef struct _TwitterChatContext TwitterChatContext;
typedef void (*TwitterChatLeaveFunc)(TwitterChatContext *ctx);
typedef gint (*TwitterChatSendMessageFunc)(TwitterChatContext *ctx, const char *message);
struct _TwitterChatContext
{
	TwitterChatType type;
	PurpleAccount *account;
	guint timer_handle;
	guint chat_id;
	TwitterChatLeaveFunc leave_cb;
	TwitterChatSendMessageFunc send_message_cb;
};

typedef struct
{
	TwitterChatContext base;

	gchar *search_text; /* e.g. N900 */
	gchar *refresh_url; /* e.g. ?since_id=6276370030&q=n900 */

	long long last_tweet_id;
} TwitterSearchTimeoutContext;

typedef struct
{
	TwitterChatContext base;
	guint timeline_id;
} TwitterTimelineTimeoutContext;

typedef struct
{
	guint get_replies_timer;
	guint get_friends_timer;
	long long last_reply_id;
	long long last_home_timeline_id;
	long long failed_get_replies_count;

	//TODO, combine
	GHashTable *search_chat_ids;
	GHashTable *timeline_chat_ids;
	/* key: gchar *screen_name,
	 * value: gchar *reply_id (then converted to long long)
	 * Store the id of last reply sent from any user to @me
	 * This is used as in_reply_to_status_id
	 * when @me sends a tweet to others */
	GHashTable *user_reply_id_table;

	gboolean requesting;
} TwitterConnectionData;

static long long purple_account_get_long_long(PurpleAccount *account, const gchar *key, long long default_value)
{
	const char* tmp_str;

	tmp_str = purple_account_get_string(account, key, NULL);
	if(tmp_str)
		return strtoll(tmp_str, NULL, 10);
	else
		return 0;
}

static void purple_account_set_long_long(PurpleAccount *account, const gchar *key, long long value)
{
	gchar* tmp_str;

	tmp_str = g_strdup_printf("%lld", value);
	purple_account_set_string(account, key, tmp_str);
	g_free(tmp_str);
}


//TODO: lots of copy and pasting here... let's refactor next
static long long twitter_account_get_last_home_timeline_id(PurpleAccount *account)
{
	return purple_account_get_long_long(account, "twitter_last_home_timeline_id", 0);
}

static void twitter_account_set_last_home_timeline_id(PurpleAccount *account, long long reply_id)
{
	purple_account_set_long_long(account, "twitter_last_home_timeline_id", reply_id);
}

static long long twitter_connection_get_last_home_timeline_id(PurpleConnection *gc)
{
	long long reply_id = 0;
	TwitterConnectionData *connection_data = gc->proto_data;
	reply_id = connection_data->last_home_timeline_id;
	return (reply_id ? reply_id : twitter_account_get_last_home_timeline_id(purple_connection_get_account(gc)));
}

static void twitter_connection_set_last_home_timeline_id(PurpleConnection *gc, long long reply_id)
{
	TwitterConnectionData *connection_data = gc->proto_data;

	connection_data->last_home_timeline_id = reply_id;
	twitter_account_set_last_home_timeline_id(purple_connection_get_account(gc), reply_id);
}

static long long twitter_account_get_last_reply_id(PurpleAccount *account)
{
	return purple_account_get_long_long(account, "twitter_last_reply_id", 0);
}

static void twitter_account_set_last_reply_id(PurpleAccount *account, long long reply_id)
{
	purple_account_set_long_long(account, "twitter_last_reply_id", reply_id);
}

static long long twitter_connection_get_last_reply_id(PurpleConnection *gc)
{
	long long reply_id = 0;
	TwitterConnectionData *connection_data = gc->proto_data;
	reply_id = connection_data->last_reply_id;
	return (reply_id ? reply_id : twitter_account_get_last_reply_id(purple_connection_get_account(gc)));
}

static void twitter_connection_set_last_reply_id(PurpleConnection *gc, long long reply_id)
{
	TwitterConnectionData *connection_data = gc->proto_data;

	connection_data->last_reply_id = reply_id;
	twitter_account_set_last_reply_id(purple_connection_get_account(gc), reply_id);
}

//borrowed almost exactly from msn
//There's still something wrong here...
//Need to fix this for people who aren't in +0000
static time_t twitter_status_parse_timestamp(const char *timestamp)
{
	//Sat Mar 07 18:12:10 +0000 2009
	char month_str[4], tz_str[6];
	char day_name[4];
	char *tz_ptr = tz_str;
	static const char *months[] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec", NULL
	};
	time_t tval = 0;
	struct tm t;
	memset(&t, 0, sizeof(t));

	time(&tval);
	localtime_r(&tval, &t);

	if (sscanf(timestamp, "%03s %03s %02d %02d:%02d:%02d %05s %04d",
				day_name, month_str, &t.tm_mday,
				&t.tm_hour, &t.tm_min, &t.tm_sec,
				tz_str, &t.tm_year) == 8) {
		gboolean offset_positive = TRUE;
		int tzhrs;
		int tzmins;

		for (t.tm_mon = 0;
				months[t.tm_mon] != NULL &&
				strcmp(months[t.tm_mon], month_str) != 0; t.tm_mon++);
		if (months[t.tm_mon] != NULL) {
			if (*tz_str == '-') {
				offset_positive = FALSE;
				tz_ptr++;
			} else if (*tz_str == '+') {
				tz_ptr++;
			}

			if (sscanf(tz_ptr, "%02d%02d", &tzhrs, &tzmins) == 2) {
				time_t tzoff = tzhrs * 60 * 60 + tzmins * 60;
#ifdef _WIN32
				long sys_tzoff;
#endif

				if (offset_positive)
					tzoff *= -1;

				t.tm_year -= 1900;

#ifdef _WIN32
				if ((sys_tzoff = wpurple_get_tz_offset()) != -1)
					tzoff += sys_tzoff;
#else
#ifdef HAVE_TM_GMTOFF
				tzoff += t.tm_gmtoff;
#else
#	ifdef HAVE_TIMEZONE
				tzset();    /* making sure */
				tzoff -= timezone;
#	endif
#endif
#endif /* _WIN32 */

				return mktime(&t) + tzoff;
			}
		}
	}

	purple_debug_info(TWITTER_PROTOCOL_ID, "Can't parse timestamp %s\n", timestamp);
	return tval;
}

static void twitter_user_data_free(TwitterUserData *user_data)
{
	if (!user_data)
		return;
	if (user_data->name)
		g_free(user_data->name);
	if (user_data->screen_name)
		g_free(user_data->screen_name);
	if (user_data->profile_image_url)
		g_free(user_data->profile_image_url);
	if (user_data->description)
		g_free(user_data->description);
	g_free(user_data);
	user_data = NULL;
}

static void twitter_status_data_free(TwitterStatusData *status)
{
	if (status == NULL)
		return;

	if (status->text != NULL)
		g_free(status->text);
	status->text = NULL;

	if (status->in_reply_to_screen_name)
		g_free (status->in_reply_to_screen_name);
	status->in_reply_to_screen_name = NULL;

	g_free(status);
}

static TwitterBuddyData *twitter_buddy_get_buddy_data(PurpleBuddy *b)
{
	if (b->proto_data == NULL)
		b->proto_data = g_new0(TwitterBuddyData, 1);
	return b->proto_data;
}

static const char *twitter_status_text_get_text(const char *text)
{
	if (text[0] != '@')
	{
		return text;
	} else {
		return strchr(text, ' ') + 1;
	}
}

static void twitter_buddy_update_icon_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
	PurpleBuddy *buddy = user_data;
	TwitterBuddyData *buddy_data = buddy->proto_data;
	if (buddy_data != NULL && buddy_data->user != NULL)
	{
		purple_buddy_icons_set_for_user(buddy->account, buddy->name,
				g_memdup(url_text, len), len, buddy_data->user->profile_image_url);
	}
}
static void twitter_buddy_update_icon(PurpleBuddy *buddy)
{
	const char *previous_url = purple_buddy_icons_get_checksum_for_user(buddy);
	TwitterBuddyData *twitter_buddy_data = buddy->proto_data;
	if (twitter_buddy_data == NULL || twitter_buddy_data->user == NULL)
	{
		return;
	} else {
		const char *url = twitter_buddy_data->user->profile_image_url;
		if (url == NULL)
		{
			purple_buddy_icons_set_for_user(buddy->account, buddy->name, NULL, 0, NULL);
		} else {
			if (!previous_url || !g_str_equal(previous_url, url)) {
				purple_util_fetch_url(url, TRUE, NULL, FALSE, twitter_buddy_update_icon_cb, buddy);
			}
		}
	}
}

/******************************************************
 *  Chat
 ******************************************************/
static GList *twitter_chat_info(PurpleConnection *gc) {
	struct proto_chat_entry *pce; /* defined in prpl.h */
	GList *chat_info = NULL;

	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = "Search";
	pce->identifier = "search";
	pce->required = TRUE;

	chat_info = g_list_append(chat_info, pce);

	pce = g_new0(struct proto_chat_entry, 1);
	pce->label = "Update Interval";
	pce->identifier = "interval";
	pce->required = TRUE;
	pce->is_int = TRUE;
	pce->min = 1;
	pce->max = 60;

	chat_info = g_list_append(chat_info, pce);

	return chat_info;
}

GHashTable *twitter_chat_info_defaults(PurpleConnection *gc, const char *chat_name)
{
	GHashTable *defaults;

	defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	g_hash_table_insert(defaults, "search", g_strdup(chat_name));

	//bug in pidgin prevents this from working
	g_hash_table_insert(defaults, "interval",
			g_strdup_printf("%d", purple_account_get_int(purple_connection_get_account(gc),
					TWITTER_PREF_SEARCH_TIMEOUT, TWITTER_PREF_SEARCH_TIMEOUT_DEFAULT)));
	return defaults;
}

//Taken mostly from blist.c
static PurpleChat *_twitter_blist_chat_find(PurpleAccount *account, TwitterChatType chat_type,
	const char *component_key, const char *component_value)
{
	const char *node_chat_name;
	gint node_chat_type = 0;
	const char *node_chat_type_str;
	PurpleChat *chat;
	PurpleBlistNode *node, *group;
	char *normname;
	PurpleBuddyList *purplebuddylist = purple_get_blist();
	GHashTable *components;

	g_return_val_if_fail(purplebuddylist != NULL, NULL);
	g_return_val_if_fail((component_value != NULL) && (*component_value != '\0'), NULL);

	normname = g_strdup(purple_normalize(account, component_value));
	purple_debug_info(TWITTER_PROTOCOL_ID, "Account %s searching for chat %s type %d\n",
		account->username,
		component_value == NULL ? "NULL" : component_value,
		chat_type);

	if (normname == NULL)
	{
		return NULL;
	}
	for (group = purplebuddylist->root; group != NULL; group = group->next) {
		for (node = group->child; node != NULL; node = node->next) {
			if (PURPLE_BLIST_NODE_IS_CHAT(node)) {

				chat = (PurpleChat*)node;

				if (account != chat->account)
					continue;

				components = purple_chat_get_components(chat);
				if (components != NULL)
				{
					node_chat_type_str = g_hash_table_lookup(components, "chat_type");
					node_chat_name = g_hash_table_lookup(components, component_key);
					node_chat_type = node_chat_type_str == NULL ? 0 : strtol(node_chat_type_str, NULL, 10);

					if (node_chat_name != NULL && node_chat_type == chat_type && !strcmp(purple_normalize(account, node_chat_name), normname))
					{
						g_free(normname);
						return chat;
					}
				}
			}
		}
	}

	g_free(normname);
	return NULL;
}

static PurpleChat *twitter_blist_chat_find_search(PurpleAccount *account, const char *name)
{
	return _twitter_blist_chat_find(account, TWITTER_CHAT_SEARCH, "search", name);
}
static PurpleChat *twitter_blist_chat_find_timeline(PurpleAccount *account, gint timeline_id)
{
	char *tmp = g_strdup_printf("%d", timeline_id);
	PurpleChat *chat = _twitter_blist_chat_find(account, TWITTER_CHAT_TIMELINE, "timeline_id", tmp);
	g_free(tmp);
	return chat;
}

static PurpleChat *twitter_blist_chat_timeline_new(PurpleAccount *account, gint timeline_id)
{
	PurpleGroup *g;
	PurpleChat *c = twitter_blist_chat_find_timeline(account, timeline_id);
	GHashTable *components;
	if (c != NULL)
	{
		return c;
	}
	g = purple_find_group("twitter");
	if (g == NULL)
		g = purple_group_new("twitter");

	components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

	//TODO
	//bug in pidgin prevents this from working
	g_hash_table_insert(components, "interval",
			g_strdup_printf("%d", purple_account_get_int(account,
					TWITTER_PREF_SEARCH_TIMEOUT, TWITTER_PREF_SEARCH_TIMEOUT_DEFAULT)));
	g_hash_table_insert(components, "chat_type",
			g_strdup_printf("%d", TWITTER_CHAT_TIMELINE));
	g_hash_table_insert(components, "timeline_id",
			g_strdup_printf("%d", timeline_id));

	c = purple_chat_new(account, "Home Timeline", components);
	purple_blist_add_chat(c, g, NULL);
	return c;
}

static PurpleChat *twitter_blist_chat_new(PurpleAccount *account, const char *searchtext)
{
	PurpleGroup *g;
	PurpleChat *c = twitter_blist_chat_find_search(account, searchtext);
	GHashTable *components;
	if (c != NULL)
	{
		return c;
	}
	g = purple_find_group("twitter");
	if (g == NULL)
		g = purple_group_new("twitter");

	components = twitter_chat_info_defaults(purple_account_get_connection(account), searchtext);

	c = purple_chat_new(account, searchtext, components);
	purple_blist_add_chat(c, g, NULL);
	return c;
}
static PurpleBuddy *twitter_buddy_new(PurpleAccount *account, const char *screenname, const char *alias)
{
	PurpleGroup *g;
	PurpleBuddy *b = purple_find_buddy(account, screenname);
	if (b != NULL)
	{
		if (b->proto_data == NULL)
			b->proto_data = g_new0(TwitterBuddyData, 1);
		return b;
	}

	g = purple_find_group("twitter");
	if (g == NULL)
		g = purple_group_new("twitter");
	b = purple_buddy_new(account, screenname, alias);
	purple_blist_add_buddy(b, NULL, g, NULL);
	b->proto_data = g_new0(TwitterBuddyData, 1);
	return b;
}

static void twitter_buddy_set_user_data(PurpleAccount *account, TwitterUserData *u, gboolean add_missing_buddy)
{
	PurpleBuddy *b;
	TwitterBuddyData *buddy_data;
	if (!u || !account)
		return;

	if (!strcmp(u->screen_name, account->username))
	{
		twitter_user_data_free(u);
		return;
	}

	b = purple_find_buddy(account, u->screen_name);
	if (!b && add_missing_buddy)
	{
		/* set alias as screenname (name) */
		gchar *alias = g_strdup_printf ("%s | %s", u->name, u->screen_name);
		b = twitter_buddy_new(account, u->screen_name, alias);
		g_free (alias);
	}

	if (!b)
	{
		twitter_user_data_free(u);
		return;
	}

	buddy_data = twitter_buddy_get_buddy_data(b);

	if (buddy_data == NULL)
		return;
	if (buddy_data->user != NULL && u != buddy_data->user)
		twitter_user_data_free(buddy_data->user);
	buddy_data->user = u;
	twitter_buddy_update_icon(b);
}

static void twitter_status_data_update_conv(PurpleAccount *account,
		char *src_user,
		TwitterStatusData *s,
		gboolean add_link)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	gchar *tweet;

	if (!s || !s->text)
		return;

	if (s->id && s->id > twitter_connection_get_last_reply_id(gc))
	{
		twitter_connection_set_last_reply_id(gc, s->id);
	}

	//Account received an im
	/* TODO get in_reply_to_status? s->in_reply_to_screen_name
	 * s->in_reply_to_status_id */

	if (add_link) {
		tweet = g_strdup_printf ("%s\nhttp://twitter.com/%s/status/%lld\n",
				twitter_status_text_get_text(s->text),
				src_user,
				s->id);
	}
	else {
		tweet = g_strdup_printf ("%s", twitter_status_text_get_text(s->text));
	}

	serv_got_im(gc, src_user,
			tweet,
			PURPLE_MESSAGE_RECV, time(NULL)/*s->created_at*/); //TODO: once timestamp parsing is fixed...

	g_free (tweet);
}

static void twitter_buddy_set_status_data(PurpleAccount *account, char *src_user, TwitterStatusData *s)
{
	PurpleBuddy *b;
	TwitterBuddyData *buddy_data;
	gboolean status_text_same = FALSE;

	if (!s)
		return;

	if (!s->text)
	{
		twitter_status_data_free(s);
		return;
	}


	b = purple_find_buddy(account, src_user);
	if (!b)
	{
		twitter_status_data_free(s);
		return;
	}

	buddy_data = twitter_buddy_get_buddy_data(b);

	if (buddy_data->status && s->created_at < buddy_data->status->created_at)
	{
		twitter_status_data_free(s);
		return;
	}

	if (buddy_data->status != NULL && s != buddy_data->status)
	{
		status_text_same = (strcmp(buddy_data->status->text, s->text) == 0);
		twitter_status_data_free(buddy_data->status);
	}

	buddy_data->status = s;

	if (!status_text_same)
	{
		purple_prpl_got_user_status(b->account, b->name, "online",
				"message", s ? s->text : NULL, NULL);
	}
}

static TwitterUserData *twitter_user_node_parse(xmlnode *user_node)
{
	TwitterUserData *user;

	if (user_node == NULL)
		return NULL;

	user = g_new0(TwitterUserData, 1);
	user->screen_name = xmlnode_get_child_data(user_node, "screen_name");

	if (!user->screen_name)
	{
		g_free(user);
		return NULL;
	}

	user->name = xmlnode_get_child_data(user_node, "name");
	user->profile_image_url = xmlnode_get_child_data(user_node, "profile_image_url");
	user->description = xmlnode_get_child_data(user_node, "description");

	return user;
}
static TwitterStatusData *twitter_status_node_parse(xmlnode *status_node)
{
	TwitterStatusData *status;
	char *data;

	if (status_node == NULL)
		return NULL;

	status = g_new0(TwitterStatusData, 1);
	status->text = xmlnode_get_child_data(status_node, "text");

	if ((data = xmlnode_get_child_data(status_node, "created_at")))
	{
		time_t created_at = twitter_status_parse_timestamp(data);
		status->created_at = created_at ? created_at : time(NULL);
		g_free(data);
	}

	if ((data = xmlnode_get_child_data(status_node, "id")))
	{
		status->id = strtoll(data, NULL, 10);
		g_free(data);
	}

	if ((data = xmlnode_get_child_data(status_node, "in_reply_to_status_id"))) {
		status->in_reply_to_status_id = strtoll (data, NULL, 10);
		g_free (data);
	}

	status->in_reply_to_screen_name =
		xmlnode_get_child_data(status_node, "in_reply_to_screen_name");

	return status;

}

static GList *twitter_users_node_parse(xmlnode *users_node)
{
	GList *users = NULL;
	xmlnode *user_node;
	for (user_node = users_node->child; user_node; user_node = user_node->next)
	{
		if (user_node->name && !strcmp(user_node->name, "user"))
		{
			TwitterBuddyData *data = g_new0(TwitterBuddyData, 1);

			xmlnode *status_node = xmlnode_get_child(user_node, "status");

			data->user = twitter_user_node_parse(user_node);
			data->status = twitter_status_node_parse(status_node);

			users = g_list_append(users, data);
		}
	}
	return users;
}
static GList *twitter_users_nodes_parse(GList *nodes)
{
	GList *l_users_data = NULL;
	GList *l;
	for (l = nodes; l; l = l->next)
	{
		xmlnode *node = l->data;
		l_users_data = g_list_concat(twitter_users_node_parse(node), l_users_data);
	}
	return l_users_data;
}

static GList *twitter_statuses_node_parse(xmlnode *statuses_node)
{
	GList *statuses = NULL;
	xmlnode *status_node;

	for (status_node = statuses_node->child; status_node; status_node = status_node->next)
	{
		if (status_node->name && !strcmp(status_node->name, "status"))
		{
			TwitterBuddyData *data = g_new0(TwitterBuddyData, 1);
			xmlnode *user_node = xmlnode_get_child(status_node, "user");
			data->user = twitter_user_node_parse(user_node);
			data->status = twitter_status_node_parse(status_node);
			statuses = g_list_prepend(statuses, data);
		}
	}

	return statuses;
}

static GList *twitter_statuses_nodes_parse(GList *nodes)
{
	GList *l_users_data = NULL;
	GList *l;
	for (l = nodes; l; l = l->next)
	{
		xmlnode *node = l->data;
		l_users_data = g_list_concat(twitter_statuses_node_parse(node), l_users_data);
	}
	return l_users_data;
}

static void twitter_verify_connection_error_handler(PurpleAccount *account, const TwitterRequestErrorData *error_data)
{
	const gchar *error_message;
	PurpleConnectionError reason;
	//purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_data->message);
	switch (error_data->type)
	{
		case TWITTER_REQUEST_ERROR_SERVER:
			reason = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
			error_message = error_data->message;
			break;
		case TWITTER_REQUEST_ERROR_INVALID_XML:
			reason = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
			error_message = "Received Invalid XML";
			break;
		case TWITTER_REQUEST_ERROR_TWITTER_GENERAL:
			if (!strcmp(error_data->message, "This method requires authentication."))
			{
				reason = PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED;
				error_message = "Invalid password";
				break;
			} else {
				reason = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
				error_message = error_data->message;
			}
			break;
		default:
			reason = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
			error_message = "Unknown error";
			break;
	}
	purple_connection_error_reason(purple_account_get_connection(account), reason, error_message);
}
static gboolean twitter_get_friends_verify_error_cb(PurpleAccount *account,
		const TwitterRequestErrorData *error_data,
		gpointer user_data)
{
	twitter_verify_connection_error_handler(account, error_data);
	return FALSE;
}

/******************************************************
 *  Twitter friends
 ******************************************************/

static void twitter_buddy_datas_set_all(PurpleAccount *account, GList *buddy_datas)
{
	GList *l;
	for (l = buddy_datas; l; l = l->next)
	{
		TwitterBuddyData *data = l->data;
		TwitterUserData *user = data->user;
		TwitterStatusData *status = data->status;
		char *screen_name = g_strdup(user->screen_name);

		g_free(data);

		twitter_buddy_set_user_data(account, user, TRUE);
		if (status != NULL)
			twitter_buddy_set_status_data(account, screen_name, status);

		g_free(screen_name);
	}
	g_list_free(buddy_datas);
}

static void twitter_get_friends_cb(PurpleAccount *account, GList *nodes, gpointer user_data)
{
	GList *buddy_datas = twitter_users_nodes_parse(nodes);
	twitter_buddy_datas_set_all(account, buddy_datas);
}

static gboolean twitter_get_friends_timeout(gpointer data)
{
	PurpleAccount *account = data;
	//TODO handle errors
	twitter_api_get_friends(account, twitter_get_friends_cb, NULL, NULL);
	return TRUE;
}

/******************************************************
 *  Twitter relies/mentions
 ******************************************************/

static void _process_replies (PurpleAccount *account,
		GList *statuses,
		TwitterConnectionData *twitter)
{
	GList *l;
	gboolean add_link = purple_account_get_bool (account,
			TWITTER_PREF_ADD_URL_TO_TWEET,
			TWITTER_PREF_ADD_URL_TO_TWEET_DEFAULT);

	for (l = statuses; l; l = l->next)
	{
		TwitterBuddyData *data = l->data;
		TwitterStatusData *status = data->status;
		TwitterUserData *user_data = data->user;
		g_free(data);

		if (!user_data)
		{
			twitter_status_data_free(status);
		} else {
			char *screen_name = g_strdup(user_data->screen_name);
			twitter_buddy_set_user_data(account, user_data, FALSE);
			twitter_status_data_update_conv(account, screen_name, status, add_link);
			twitter_buddy_set_status_data(account, screen_name, status);

			/* update user_reply_id_table table */
			gchar *reply_id = g_strdup_printf ("%lld", status->id);
			g_hash_table_insert (twitter->user_reply_id_table,
					g_strdup (screen_name), reply_id);
			g_free(screen_name);
		}
	}

	twitter->failed_get_replies_count = 0;
	twitter->requesting = FALSE;
}

static void twitter_get_replies_timeout_error_cb (PurpleAccount *account,
		const TwitterRequestErrorData *error_data,
		gpointer user_data)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	TwitterConnectionData *twitter = gc->proto_data;
	twitter->failed_get_replies_count++;

	if (twitter->failed_get_replies_count >= 3)
	{
		purple_connection_error_reason(gc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR, "Could not retrieve replies, giving up trying");
	}

	twitter->requesting = FALSE;
}

static void twitter_get_replies_cb (PurpleAccount *account,
		xmlnode *node,
		gpointer user_data)
{
	PurpleConnection *gc = purple_account_get_connection (account);
	TwitterConnectionData *twitter = gc->proto_data;

	GList *statuses = twitter_statuses_node_parse (node);
	_process_replies (account, statuses, twitter);

	g_list_free(statuses);
}

static gboolean twitter_get_replies_all_timeout_error_cb (PurpleAccount *account,
		const TwitterRequestErrorData *error_data,
		gpointer user_data)
{
	twitter_get_replies_timeout_error_cb (account, error_data, user_data);
	return FALSE;
}

static void twitter_chat_add_tweet(PurpleConvChat *chat, const char *who, const char *message, time_t time)
{
	if (!purple_conv_chat_find_user(chat, who))
	{
		purple_conv_chat_add_user(chat,
				who,
				NULL,   /* user-provided join message, IRC style */
				PURPLE_CBFLAGS_NONE,
				FALSE);  /* show a join message */
	}
	serv_got_chat_in(purple_conversation_get_gc(purple_conv_chat_get_conversation(chat)),
			purple_conv_chat_get_id(chat),
			who,
			PURPLE_MESSAGE_RECV,
			message,
			time);
}

static void twitter_get_home_timeline_parse_statuses(PurpleAccount *account,
		TwitterTimelineTimeoutContext *ctx, GList *statuses)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	PurpleConvChat *chat;
	PurpleConversation *conv;
	GList *l;
	g_return_if_fail(account != NULL);
	g_return_if_fail(statuses != NULL);

	conv = purple_find_chat(gc, ctx->base.chat_id);
	g_return_if_fail (conv != NULL); //todo: destroy context

	chat = PURPLE_CONV_CHAT(conv);

	for (l = statuses; l; l = l->next)
	{
		TwitterBuddyData *data = l->data;
		TwitterStatusData *status = data->status;
		TwitterUserData *user_data = data->user;
		g_free(data);

		if (!user_data)
		{
			twitter_status_data_free(status);
		} else {
			const char *screen_name = user_data->screen_name;
			const char *text = status->text;
			twitter_chat_add_tweet(chat, screen_name, text, time(NULL));
			if (status->id && status->id > twitter_connection_get_last_home_timeline_id(gc))
			{
				twitter_connection_set_last_home_timeline_id(gc, status->id);
			}

			//if (status->id >
			//twitter_buddy_set_user_data(account, user_data, FALSE);
			//twitter_status_data_update_conv(account, screen_name, status, add_link);
			//twitter_buddy_set_status_data(account, screen_name, status);

			/* update user_reply_id_table table */
			//gchar *reply_id = g_strdup_printf ("%lld", status->id);
			//g_hash_table_insert (twitter->user_reply_id_table,
					//g_strdup (screen_name), reply_id);
			//g_free(screen_name);
		}
	}
}

static void twitter_get_home_timeline_cb(PurpleAccount *account, xmlnode *node, gpointer user_data)
{
	TwitterTimelineTimeoutContext *ctx = (TwitterTimelineTimeoutContext *)user_data;

	purple_debug_info(TWITTER_PROTOCOL_ID, "%s\n", G_STRFUNC);

	GList *statuses = twitter_statuses_node_parse(node);
	twitter_get_home_timeline_parse_statuses(account, ctx, statuses);
	g_list_free(statuses);

}

static void twitter_get_home_timeline_all_cb(PurpleAccount *account,
		GList *nodes,
		gpointer user_data)
{
	TwitterTimelineTimeoutContext *ctx = (TwitterTimelineTimeoutContext *)user_data;

	purple_debug_info(TWITTER_PROTOCOL_ID, "%s\n", G_STRFUNC);

	GList *statuses = twitter_statuses_nodes_parse(nodes);
	twitter_get_home_timeline_parse_statuses(account, ctx, statuses);
	g_list_free(statuses);
}
static void twitter_get_replies_all_cb (PurpleAccount *account,
		GList *nodes,
		gpointer user_data)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	TwitterConnectionData *twitter = gc->proto_data;

	GList *statuses = twitter_statuses_nodes_parse(nodes);
	_process_replies (account, statuses, twitter);

	g_list_free(statuses);
}

static gboolean twitter_get_replies_timeout (gpointer data)
{
	PurpleAccount *account = data;
	PurpleConnection *gc = purple_account_get_connection(account);
	TwitterConnectionData *twitter = gc->proto_data;

	if (!twitter->requesting) {
		twitter->requesting = TRUE;
		twitter_api_get_replies_all(account,
				twitter_connection_get_last_reply_id(purple_account_get_connection(account)),
				twitter_get_replies_all_cb, twitter_get_replies_all_timeout_error_cb,
				NULL);
	}
	return TRUE;
}


/******************************************************
 *  Twitter search
 ******************************************************/
static void _twitter_chat_context_destroy (TwitterChatContext *ctx)
{
	if (ctx->timer_handle)
	{
		purple_timeout_remove(ctx->timer_handle);
		ctx->timer_handle = 0;
	}
}

static void twitter_chat_search_leave(TwitterChatContext *ctx_base)
{
	PurpleConnection *gc;
	TwitterSearchTimeoutContext *ctx;
	g_return_if_fail(ctx_base != NULL);
	ctx = (TwitterSearchTimeoutContext *) ctx_base;
	gc = purple_account_get_connection(ctx->base.account);
	TwitterConnectionData *twitter = gc->proto_data;

	g_hash_table_remove(twitter->search_chat_ids, ctx->search_text);
	_twitter_chat_context_destroy(&ctx->base);

	ctx->last_tweet_id = 0;

	g_free (ctx->search_text);
	ctx->search_text = NULL;

	g_free (ctx->refresh_url);
	ctx->refresh_url = NULL;

	g_slice_free (TwitterSearchTimeoutContext, ctx);
} 

static void twitter_chat_timeline_leave(TwitterChatContext *ctx_base)
{
	PurpleConnection *gc;
	TwitterTimelineTimeoutContext *ctx;
	g_return_if_fail(ctx_base != NULL);
	ctx = (TwitterTimelineTimeoutContext *) ctx_base;
	gc = purple_account_get_connection(ctx->base.account);
	TwitterConnectionData *twitter = gc->proto_data;

	g_hash_table_remove(twitter->timeline_chat_ids, &ctx->timeline_id);
	_twitter_chat_context_destroy(&ctx->base);

	g_slice_free (TwitterTimelineTimeoutContext, ctx);
} 



static PurpleConversation *twitter_conv_timeline_find(PurpleConnection *gc, const gint timeline_id)
{
	TwitterConnectionData *twitter = gc->proto_data;
	gint *chat_id = g_hash_table_lookup(twitter->timeline_chat_ids, &timeline_id);
	if (chat_id == NULL)
		return NULL;
	else
		return purple_find_chat(gc, *chat_id);
}
static PurpleConversation *twitter_conv_search_find(PurpleConnection *gc, const gchar *name)
{
	TwitterConnectionData *twitter = gc->proto_data;
	gint *chat_id = g_hash_table_lookup(twitter->search_chat_ids, name);
	if (chat_id == NULL)
		return NULL;
	else
		return purple_find_chat(gc, *chat_id);
}
static void twitter_search_cb(PurpleAccount *account,
		const GArray *search_results,
		const gchar *refresh_url,
		long long max_id,
		gpointer user_data)
{
	TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *)user_data;
	PurpleConnection *gc = purple_account_get_connection(account);
	gint i, len = search_results->len;
	gboolean add_link;
	PurpleConversation *conv;
	PurpleConvChat *chat;

	g_return_if_fail (ctx != NULL);
	//
	//TODO DEBUG stuff

	conv = purple_find_chat(gc, ctx->base.chat_id);
	g_return_if_fail (conv != NULL); //destroy context

	chat = PURPLE_CONV_CHAT(conv);

	add_link = purple_account_get_bool (account,
			TWITTER_PREF_ADD_URL_TO_TWEET,
			TWITTER_PREF_ADD_URL_TO_TWEET_DEFAULT);

	for (i = len-1; i >= 0; i--) {
		TwitterSearchData *search_data;
		gchar *tweet;

		search_data = g_array_index (search_results,
				TwitterSearchData *, i);

		if (add_link) {
			tweet = g_strdup_printf (
					"%s\nhttp://twitter.com/%s/status/%lld\n",
					search_data->text,
					search_data->from_user,
					search_data->id);
		}
		else {
			tweet = g_strdup(search_data->text);
		}

		twitter_chat_add_tweet(chat, search_data->from_user, tweet, time(NULL));//TODO: FIX TIME

		g_free (tweet);
	}

	ctx->last_tweet_id = max_id;
	g_free (ctx->refresh_url);
	ctx->refresh_url = g_strdup (refresh_url);
}

static gboolean twitter_search_timeout(gpointer data)
{
	TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *)data;

	if (ctx->refresh_url) {
		purple_debug_info(TWITTER_PROTOCOL_ID, "%s, refresh_url exists: %s\n",
				G_STRFUNC, ctx->refresh_url);

		twitter_api_search_refresh(ctx->base.account, ctx->refresh_url,
				twitter_search_cb, NULL, ctx);
	}
	else {
		gchar *refresh_url;

		refresh_url = g_strdup_printf ("?q=%s&since_id=%lld",
				purple_url_encode(ctx->search_text), ctx->last_tweet_id);

		purple_debug_info(TWITTER_PROTOCOL_ID, "%s, create refresh_url: %s\n",
				G_STRFUNC, refresh_url);

		twitter_api_search_refresh (ctx->base.account, refresh_url,
				twitter_search_cb, NULL, ctx);

		g_free (refresh_url);
	}

	return TRUE;
}

static int twitter_chat_timeline_send(TwitterChatContext *ctx_base, const gchar *message)
{
	PurpleAccount *account = ctx_base->account;
	PurpleConnection *gc = purple_account_get_connection(account);
	//TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *) ctx_base;
	PurpleConversation *conv = purple_find_chat(gc, ctx_base->chat_id);

	if (conv == NULL) return -1; //TODO: error?

	if (strlen(message) > MAX_TWEET_LENGTH)
	{
		//TODO: SHOW ERROR
		return -1;
	}
	else
	{
		twitter_api_set_status(account,
				message, 0,
				NULL, NULL,//TODO: verify & error
				NULL);
		twitter_chat_add_tweet(PURPLE_CONV_CHAT(conv), account->username, message, time(NULL));//TODO: FIX TIME
		return 0;
	}
}
static int twitter_chat_search_send(TwitterChatContext *ctx_base, const gchar *message)
{
	PurpleAccount *account = ctx_base->account;
	PurpleConnection *gc = purple_account_get_connection(account);
	TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *) ctx_base;
	PurpleConversation *conv = purple_find_chat(gc, ctx_base->chat_id);
	char *status;
	char *message_lower, *search_text_lower;

	if (conv == NULL) return -1; //TODO: error?

	//TODO: verify that we want utf8 case insensitive, and not ascii
	message_lower = g_utf8_strdown(message, -1);
	search_text_lower = g_utf8_strdown(ctx->search_text, -1);
	if (strstr(message_lower, search_text_lower))
	{
		status = g_strdup(message);
	} else {
		status = g_strdup_printf("%s %s", ctx->search_text, message);
	}
	g_free(message_lower);
	g_free(search_text_lower);

	if (strlen(status) > MAX_TWEET_LENGTH)
	{
		//TODO: SHOW ERROR
		g_free(status);
		return -1;
	}
	else
	{
		PurpleAccount *account = purple_connection_get_account(gc);
		twitter_api_set_status(purple_connection_get_account(gc),
				status, 0,
				NULL, NULL,//TODO: verify & error
				NULL);
		twitter_chat_add_tweet(PURPLE_CONV_CHAT(conv), account->username, status, time(NULL));//TODO: FIX TIME
		g_free(status);
		return 0;
	}
}

static void twitter_chat_context_new(TwitterChatContext *ctx,
	TwitterChatType type, PurpleAccount *account, guint chat_id,
	TwitterChatLeaveFunc leave_cb, TwitterChatSendMessageFunc send_message_cb)
{
	ctx->type = type;
	ctx->account = account;
	ctx->chat_id = chat_id;
	ctx->leave_cb = leave_cb;
	ctx->send_message_cb = send_message_cb;
}

static TwitterSearchTimeoutContext *twitter_search_timeout_context_new(PurpleAccount *account,
		const char *search_text, guint chat_id)
{
	TwitterSearchTimeoutContext *ctx = g_slice_new0(TwitterSearchTimeoutContext);

	twitter_chat_context_new(&ctx->base, TWITTER_CHAT_SEARCH, account, chat_id,
			twitter_chat_search_leave, twitter_chat_search_send);
	ctx->search_text = g_strdup(search_text);
	return ctx;
}

static TwitterTimelineTimeoutContext *twitter_timeline_timeout_context_new(PurpleAccount *account,
		guint timeline_id, guint chat_id)
{
	TwitterTimelineTimeoutContext *ctx = g_slice_new0(TwitterTimelineTimeoutContext);

	twitter_chat_context_new(&ctx->base, TWITTER_CHAT_TIMELINE, account, chat_id,
			twitter_chat_timeline_leave, twitter_chat_timeline_send);
	ctx->timeline_id = timeline_id;
	return ctx;
}

static void get_saved_searches_cb (PurpleAccount *account,
		xmlnode *node,
		gpointer user_data)
{
	xmlnode *search;

	purple_debug_info (TWITTER_PROTOCOL_ID, "%s\n", G_STRFUNC);

	for (search = node->child; search; search = search->next) {
		if (search->name && !g_strcmp0 (search->name, "saved_search")) {
			gchar *query = xmlnode_get_child_data (search, "query");

			twitter_blist_chat_new(account, query);
			g_free (query);
		}
	}
}


static void twitter_chat_leave(PurpleConnection *gc, int id) {
	PurpleConversation *conv = purple_find_chat(gc, id);
	TwitterChatContext *ctx = (TwitterChatContext *) purple_conversation_get_data(conv, "twitter-chat-context");

	g_return_if_fail(ctx != NULL);

	if (ctx->leave_cb)
		ctx->leave_cb(ctx);
}
static int twitter_chat_send(PurpleConnection *gc, int id, const char *message,
		PurpleMessageFlags flags) {
	PurpleConversation *conv = purple_find_chat(gc, id);
	TwitterChatContext *ctx = (TwitterChatContext *) purple_conversation_get_data(conv, "twitter-chat-context");

	g_return_val_if_fail(ctx != NULL, -1);

	if (ctx->send_message_cb)
		return ctx->send_message_cb(ctx, message);
	return 0;
}

static gint twitter_get_next_chat_id()
{
	static gint chat_id = 1;
	return chat_id++;
}

static void twitter_chat_search_join(PurpleConnection *gc, GHashTable *components) {
        const char *search = g_hash_table_lookup(components, "search");
        const char *interval_str = g_hash_table_lookup(components, "interval");
        int interval = 0;
        int default_interval = purple_account_get_int(purple_connection_get_account(gc),
                        TWITTER_PREF_SEARCH_TIMEOUT, TWITTER_PREF_SEARCH_TIMEOUT_DEFAULT);

        purple_debug_info(TWITTER_PROTOCOL_ID, "%s is performing search %s\n", gc->account->username, search);

        g_return_if_fail(search != NULL);

        interval = strtol(interval_str, NULL, 10);
        if (interval < 1)
                interval = default_interval;

        if (!twitter_conv_search_find(gc, search)) {
                guint chat_id = twitter_get_next_chat_id();
		TwitterConnectionData *twitter = gc->proto_data;
                PurpleAccount *account = purple_connection_get_account(gc);
                TwitterSearchTimeoutContext *ctx = twitter_search_timeout_context_new(account,
                                search, chat_id);
                PurpleConversation *conv = serv_got_joined_chat(gc, chat_id, search);

		g_hash_table_replace(twitter->search_chat_ids, g_strdup(search), g_memdup(&chat_id, sizeof(chat_id)));
                purple_conversation_set_title(conv, search);
                purple_conversation_set_data(conv, "twitter-chat-context", ctx);

                twitter_api_search(account,
                                search, ctx->last_tweet_id,
                                TWITTER_SEARCH_RPP_DEFAULT,
                                twitter_search_cb, NULL, ctx);


                ctx->base.timer_handle = purple_timeout_add_seconds(
                                60 * interval,
                                twitter_search_timeout, ctx);
        } else {
                char *tmp = g_strdup_printf("Search %s is already open.", search);
                purple_debug_info(TWITTER_PROTOCOL_ID, "Search %s is already open.", search);
                purple_notify_info(gc, "Perform Search", "Perform Search", tmp);
                g_free(tmp);
        }
}

static gboolean twitter_timeline_timeout(gpointer data)
{
	TwitterTimelineTimeoutContext *ctx = (TwitterTimelineTimeoutContext *)data;
	PurpleAccount *account = ctx->base.account;
	PurpleConnection *gc = purple_account_get_connection(account);
	long long since_id = twitter_connection_get_last_home_timeline_id(gc);
	if (since_id == 0)
	{
		purple_debug_info(TWITTER_PROTOCOL_ID, "Retrieving %s statuses for first time\n", account->username);
		twitter_api_get_home_timeline(account,
				since_id,
				20,
				1,
				twitter_get_home_timeline_cb,
				NULL,
				ctx);
	} else {
		purple_debug_info(TWITTER_PROTOCOL_ID, "Retrieving %s statuses since %lld\n", account->username, since_id);
		twitter_api_get_home_timeline_all(account,
				since_id,
				twitter_get_home_timeline_all_cb,
				NULL,
				ctx);
	}

	return TRUE;
}

static void twitter_chat_timeline_join(PurpleConnection *gc, GHashTable *components) {
	guint timeline_id = 0;
	int interval = 1;

	purple_debug_info(TWITTER_PROTOCOL_ID, "%s is joined timeline\n", gc->account->username);

	if (!twitter_conv_timeline_find(gc, timeline_id)) {
		PurpleAccount *account = purple_connection_get_account(gc);
		TwitterConnectionData *twitter = gc->proto_data;
		PurpleConversation *conv;

		guint chat_id = twitter_get_next_chat_id();
		TwitterTimelineTimeoutContext *ctx = twitter_timeline_timeout_context_new(
				account, timeline_id, chat_id);

		conv = serv_got_joined_chat(gc, chat_id, "Home Timeline");
		ctx->base.chat_id = chat_id;
		ctx->base.account = account;

                purple_conversation_set_data(conv, "twitter-chat-context", ctx);

		g_hash_table_replace(twitter->timeline_chat_ids, g_memdup(&timeline_id, sizeof(timeline_id)), g_memdup(&chat_id, sizeof(chat_id)));
		long long since_id = twitter_connection_get_last_home_timeline_id(gc);
		//TODO: free
		//purple_conversation_set_data(conv, "twitter-timeline-context", ctx);

		if (since_id == 0)
		{
			purple_debug_info(TWITTER_PROTOCOL_ID, "Retrieving %s statuses for first time\n", gc->account->username);
			twitter_api_get_home_timeline(account,
					since_id,
					20,
					1,
					twitter_get_home_timeline_cb,
					NULL,
					ctx);
		} else {
			purple_debug_info(TWITTER_PROTOCOL_ID, "Retrieving %s statuses since %lld\n", gc->account->username, since_id);
			twitter_api_get_home_timeline_all(account,
					since_id,
					twitter_get_home_timeline_all_cb,
					NULL,
					ctx);
		}
		//TODO: I don't think this timer should be here, but for the time being...
		ctx->base.timer_handle = purple_timeout_add_seconds(
				60 * interval,
				twitter_timeline_timeout, ctx);
		/*twitter_api_search(account,
				search, ctx->last_tweet_id,
				TWITTER_SEARCH_RPP_DEFAULT,
				twitter_search_cb, NULL, ctx);*/


		/*ctx->timer_handle = purple_timeout_add_seconds(
				60 * interval,
				twitter_search_timeout, ctx);*/
	} else {
		//char *tmp = g_strdup_printf("Search %s is already open.", search);
		//purple_debug_info(TWITTER_PROTOCOL_ID, "Search %s is already open.\n", search);
		//purple_notify_info(gc, "Perform Search", "Perform Search", tmp);
		//g_free(tmp);
	}

}
static void twitter_chat_join(PurpleConnection *gc, GHashTable *components) {
	const char *conv_type_str = g_hash_table_lookup(components, "chat_type");
	gint conv_type = conv_type_str == NULL ? 0 : strtol(conv_type_str, NULL, 10);
	switch (conv_type)
	{
		case TWITTER_CHAT_SEARCH:
			twitter_chat_search_join(gc, components);
			break;
		case TWITTER_CHAT_TIMELINE:
			twitter_chat_timeline_join(gc, components);
			break;
		default:
			purple_debug_info(TWITTER_PROTOCOL_ID, "Unknown chat type %d\n", conv_type);
	}
}


static void twitter_get_friends_verify_connection_cb(PurpleAccount *account,
		GList *nodes,
		gpointer user_data)
{
	PurpleConnection *gc = purple_account_get_connection(account);
	TwitterConnectionData *twitter = gc->proto_data;
	GList *l_users_data = NULL;

	if (purple_connection_get_state(gc) == PURPLE_CONNECTING)
	{
		purple_connection_update_progress(gc, "Connected",
				2,   /* which connection step this is */
				3);  /* total number of steps */
		purple_connection_set_state(gc, PURPLE_CONNECTED);

		l_users_data = twitter_users_nodes_parse(nodes);

		twitter_blist_chat_timeline_new(account, 0);

		/* setup buddy list */
		twitter_buddy_datas_set_all(account, l_users_data);

		/* Retrieve user's saved search queries */
		twitter_api_get_saved_searches (account,
				get_saved_searches_cb, NULL, NULL);

		/* We want to retrieve all mentions/replies since
		 * last reply we have retrieved and stored locally */
		twitter_connection_set_last_reply_id(gc,
				twitter_account_get_last_reply_id (account));

		/* Immediately retrieve replies */
		twitter->requesting = TRUE;
		twitter_api_get_replies (account,
				twitter_connection_get_last_reply_id(purple_account_get_connection(account)),
				TWITTER_INITIAL_REPLIES_COUNT, 1,
				twitter_get_replies_cb,
				twitter_get_replies_timeout_error_cb,
				NULL);

		/* Install periodic timers to retrieve replies and friend list */
		twitter->get_replies_timer = purple_timeout_add_seconds(
				60 * purple_account_get_int(account, TWITTER_PREF_REPLIES_TIMEOUT, TWITTER_PREF_REPLIES_TIMEOUT_DEFAULT),
				twitter_get_replies_timeout, account);
		twitter->get_friends_timer = purple_timeout_add_seconds(
				60 * purple_account_get_int(account, TWITTER_PREF_USER_STATUS_TIMEOUT, TWITTER_PREF_USER_STATUS_TIMEOUT_DEFAULT),
				twitter_get_friends_timeout, account);
	}
}

static void twitter_get_rate_limit_status_cb(PurpleAccount *account, xmlnode *node, gpointer user_data)
{
	/*
	 * <hash>
	 * <reset-time-in-seconds type="integer">1236529763</reset-time-in-seconds>
	 * <remaining-hits type="integer">100</remaining-hits>
	 * <hourly-limit type="integer">100</hourly-limit>
	 * <reset-time type="datetime">2009-03-08T16:29:23+00:00</reset-time>
	 * </hash>
	 */

	xmlnode *child;
	int remaining_hits = 0;
	int hourly_limit = 0;
	char *message;
	for (child = node->child; child; child = child->next)
	{
		if (child->name)
		{
			if (!strcmp(child->name, "remaining-hits"))
			{
				char *data = xmlnode_get_data_unescaped(child);
				remaining_hits = atoi(data);
				g_free(data);
			}
			else if (!strcmp(child->name, "hourly-limit"))
			{
				char *data = xmlnode_get_data_unescaped(child);
				hourly_limit = atoi(data);
				g_free(data);
			}
		}
	}
	message = g_strdup_printf("%d/%d %s", remaining_hits, hourly_limit, "Remaining");
	purple_notify_info(NULL,  /* plugin handle or PurpleConnection */
			("Rate Limit Status"),
			("Rate Limit Status"),
			(message));
	g_free(message);
}

/*
 * prpl functions
 */
static const char *twitter_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
	/* shamelessly steal (er, borrow) the meanwhile protocol icon. it's cute! */
	return "meanwhile";
}

static char *twitter_status_text(PurpleBuddy *buddy) {
	purple_debug_info(TWITTER_PROTOCOL_ID, "getting %s's status text for %s\n",
			buddy->name, buddy->account->username);

	if (purple_find_buddy(buddy->account, buddy->name)) {
		PurplePresence *presence = purple_buddy_get_presence(buddy);
		PurpleStatus *status = purple_presence_get_active_status(presence);
		const char *message = status ? purple_status_get_attr_string(status, "message") : NULL;

		if (message && strlen(message) > 0)
			return g_strdup(g_markup_escape_text(message, -1));

	}
	return NULL;
}

static void twitter_tooltip_text(PurpleBuddy *buddy,
		PurpleNotifyUserInfo *info,
		gboolean full) {

	if (PURPLE_BUDDY_IS_ONLINE(buddy))
	{
		PurplePresence *presence = purple_buddy_get_presence(buddy);
		PurpleStatus *status = purple_presence_get_active_status(presence);
		char *msg = twitter_status_text(buddy);
		purple_notify_user_info_add_pair(info, purple_status_get_name(status),
				msg);
		g_free(msg);

		if (full) {
			/*const char *user_info = purple_account_get_user_info(gc->account);
			  if (user_info)
			  purple_notify_user_info_add_pair(info, _("User info"), user_info);*/
		}

		purple_debug_info(TWITTER_PROTOCOL_ID, "showing %s tooltip for %s\n",
				(full) ? "full" : "short", buddy->name);
	}
}

/* everyone will be online
 * Future: possibly have offline mode for people who haven't updated in a while
 */
static GList *twitter_status_types(PurpleAccount *acct)
{
	GList *types = NULL;
	PurpleStatusType *type;
	PurpleStatusPrimitive status_primitives[] = {
		PURPLE_STATUS_UNAVAILABLE,
		PURPLE_STATUS_INVISIBLE,
		PURPLE_STATUS_AWAY,
		PURPLE_STATUS_EXTENDED_AWAY
	};
	int status_primitives_count = sizeof(status_primitives) / sizeof(status_primitives[0]);
	int i;

	type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, TWITTER_STATUS_ONLINE,
			TWITTER_STATUS_ONLINE, TRUE);
	purple_status_type_add_attr(type, "message", ("Online"),
			purple_value_new(PURPLE_TYPE_STRING));
	types = g_list_prepend(types, type);

	//This is a hack to get notified when another protocol goes into a different status.
	//Eg aim goes "away", we still want to get notified
	for (i = 0; i < status_primitives_count; i++)
	{
		type = purple_status_type_new(status_primitives[i], TWITTER_STATUS_ONLINE,
				TWITTER_STATUS_ONLINE, FALSE);
		purple_status_type_add_attr(type, "message", ("Online"),
				purple_value_new(PURPLE_TYPE_STRING));
		types = g_list_prepend(types, type);
	}

	type = purple_status_type_new(PURPLE_STATUS_OFFLINE, TWITTER_STATUS_OFFLINE,
			TWITTER_STATUS_OFFLINE, TRUE);
	types = g_list_prepend(types, type);

	return g_list_reverse(types);
}


/*
 * UI callbacks
 */
static void twitter_action_get_user_info(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	PurpleAccount *acct = purple_connection_get_account(gc);
	twitter_api_get_friends(acct, twitter_get_friends_cb, NULL, NULL);
}

static void twitter_request_id_ok(PurpleConnection *gc, PurpleRequestFields *fields)
{
	PurpleAccount *acct = purple_connection_get_account(gc);
	TwitterConnectionData *twitter = gc->proto_data;
	const char* str = purple_request_fields_get_string(fields, "id");
	long long id = 0;
	if(str)
		id = strtoll(str, NULL, 10);

	if (!twitter->requesting) {
		twitter->requesting = TRUE;
		twitter_api_get_replies_all(acct,
				id, twitter_get_replies_all_cb,
				twitter_get_replies_all_timeout_error_cb, NULL);
	}
}
static void twitter_action_get_replies(PurplePluginAction *action)
{
	PurpleRequestFields *request;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	PurpleConnection *gc = (PurpleConnection *)action->context;

	group = purple_request_field_group_new(NULL);

	gchar* str = g_strdup_printf("%lld", twitter_connection_get_last_reply_id(gc));
	field = purple_request_field_string_new("id", ("Minutes"), str, FALSE);
	g_free(str);
	purple_request_field_group_add_field(group, field);

	request = purple_request_fields_new();
	purple_request_fields_add_group(request, group);

	purple_request_fields(action->plugin,
			("Timeline"),
			("Set last time"),
			NULL,
			request,
			("_Set"), G_CALLBACK(twitter_request_id_ok),
			("_Cancel"), NULL,
			purple_connection_get_account(gc), NULL, NULL,
			gc);
}
static void twitter_action_set_status_ok(PurpleConnection *gc, PurpleRequestFields *fields)
{
	PurpleAccount *acct = purple_connection_get_account(gc);
	const char* status = purple_request_fields_get_string(fields, "status");
	purple_account_set_status(acct, "online", TRUE, "message", status, NULL);
}
static void twitter_action_set_status(PurplePluginAction *action)
{
	PurpleRequestFields *request;
	PurpleRequestFieldGroup *group;
	PurpleRequestField *field;
	PurpleConnection *gc = (PurpleConnection *)action->context;

	group = purple_request_field_group_new(NULL);

	field = purple_request_field_string_new("status", ("Status"), "", FALSE);
	purple_request_field_group_add_field(group, field);

	request = purple_request_fields_new();
	purple_request_fields_add_group(request, group);

	purple_request_fields(action->plugin,
			("Status"),
			("Set Account Status"),
			NULL,
			request,
			("_Set"), G_CALLBACK(twitter_action_set_status_ok),
			("_Cancel"), NULL,
			purple_connection_get_account(gc), NULL, NULL,
			gc);
}
static void twitter_action_get_rate_limit_status(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	PurpleAccount *acct = purple_connection_get_account(gc);
	twitter_api_get_rate_limit_status(acct, twitter_get_rate_limit_status_cb, NULL, NULL);
}

/* this is set to the actions member of the PurplePluginInfo struct at the
 * bottom.
 */
static GList *twitter_actions(PurplePlugin *plugin, gpointer context)
{
	GList *l = NULL;
	PurplePluginAction *action;

	action = purple_plugin_action_new("Set status", twitter_action_set_status);
	l = g_list_append(l, action);

	action = purple_plugin_action_new("Rate Limit Status", twitter_action_get_rate_limit_status);
	l = g_list_append(l, action);

	l = g_list_append(l, NULL);

	action = purple_plugin_action_new("Debug - Retrieve users", twitter_action_get_user_info);
	l = g_list_append(l, action);

	action = purple_plugin_action_new("Debug - Retrieve replies", twitter_action_get_replies);
	l = g_list_append(l, action);

	return l;
}

static void twitter_get_replies_verify_connection_cb(PurpleAccount *acct, xmlnode *node, gpointer user_data)
{
	PurpleConnection *gc = purple_account_get_connection(acct);

	if (purple_connection_get_state(gc) == PURPLE_CONNECTING)
	{
		long long id = 0;
		xmlnode *status_node = xmlnode_get_child(node, "status");
		if (status_node != NULL)
		{
			TwitterStatusData *status_data = twitter_status_node_parse(status_node);
			if (status_data != NULL)
			{
				id = status_data->id;

				twitter_status_data_free(status_data);
				if (id > twitter_account_get_last_reply_id(acct))
					twitter_account_set_last_reply_id(acct, id);
			}
		}

		purple_connection_update_progress(gc, "Connecting...",
				1,   /* which connection step this is */
				3);  /* total number of steps */

	}
	twitter_api_get_friends(acct,
			twitter_get_friends_verify_connection_cb,
			twitter_get_friends_verify_error_cb,
			NULL);
}

static void twitter_get_replies_verify_connection_error_cb(PurpleAccount *acct, const TwitterRequestErrorData *error_data, gpointer user_data)
{
	twitter_verify_connection_error_handler(acct, error_data);
}

static void twitter_verify_connection(PurpleAccount *acct)
{
	gboolean retrieve_history;

	//To verify the connection, we get the user's friends.
	//With that we'll update the buddy list and set the last known reply id

	/* If history retrieval enabled, read last reply id from config file.
	 * There's no config file, just set last reply id to 0 */
	retrieve_history = purple_account_get_bool (
			acct, TWITTER_PREF_RETRIEVE_HISTORY,
			TWITTER_PREF_RETRIEVE_HISTORY_DEFAULT);

	//If we don't have a stored last reply id, we don't want to get the entire history (EVERY reply)
	if (retrieve_history && twitter_account_get_last_reply_id(acct) != 0) {
		PurpleConnection *gc = purple_account_get_connection(acct);

		if (purple_connection_get_state(gc) == PURPLE_CONNECTING) {

			purple_connection_update_progress(gc, "Connecting...",
					1,   /* which connection step this is */
					3);  /* total number of steps */
		}

		twitter_api_get_friends(acct,
				twitter_get_friends_verify_connection_cb,
				twitter_get_friends_verify_error_cb,
				NULL);
	}
	else {
		/* Simply get the last reply */
		twitter_api_get_replies(acct,
				0, 1, 1,
				twitter_get_replies_verify_connection_cb,
				twitter_get_replies_verify_connection_error_cb,
				NULL);
	}
}

static void twitter_login(PurpleAccount *acct)
{
	PurpleConnection *gc = purple_account_get_connection(acct);
	TwitterConnectionData *twitter = g_new0(TwitterConnectionData, 1);
	gc->proto_data = twitter;

	purple_debug_info(TWITTER_PROTOCOL_ID, "logging in %s\n", acct->username);

	/* key: gchar *, value: gchar * (of a long long) */
	twitter->search_chat_ids = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, g_free);

	twitter->timeline_chat_ids = g_hash_table_new_full(
			g_int_hash, g_int_equal, g_free, g_free);

	/* key: gchar *, value: gchar * (of a long long) */
	twitter->user_reply_id_table = g_hash_table_new_full (
			g_str_hash, g_str_equal, g_free, g_free);

	/* purple wants a minimum of 2 steps */
	purple_connection_update_progress(gc, ("Connecting"),
			0,   /* which connection step this is */
			2);  /* total number of steps */


	twitter_verify_connection(acct);
}

static void twitter_close(PurpleConnection *gc)
{
	/* notify other twitter accounts */
	TwitterConnectionData *twitter = gc->proto_data;

	if (twitter->get_replies_timer)
		purple_timeout_remove(twitter->get_replies_timer);

	if (twitter->get_friends_timer)
		purple_timeout_remove(twitter->get_friends_timer);

	if (twitter->user_reply_id_table)
		g_hash_table_destroy (twitter->user_reply_id_table);
	twitter->user_reply_id_table = NULL;

	if (twitter->search_chat_ids)
		g_hash_table_destroy (twitter->search_chat_ids);
	twitter->search_chat_ids = NULL;

	if (twitter->timeline_chat_ids)
		g_hash_table_destroy (twitter->timeline_chat_ids);
	twitter->timeline_chat_ids = NULL;

	g_free(twitter);
}

static void twitter_set_status_error_cb(PurpleAccount *acct, const TwitterRequestErrorData *error_data, gpointer user_data)
{
	const char *message;
	if (error_data->type == TWITTER_REQUEST_ERROR_SERVER || error_data->type == TWITTER_REQUEST_ERROR_TWITTER_GENERAL)
	{
		message = error_data->message;
	} else if (error_data->type == TWITTER_REQUEST_ERROR_INVALID_XML) {
		message = "Unknown reply by twitter server";
	} else {
		message = "Unknown error";
	}
	purple_notify_error(NULL,  /* plugin handle or PurpleConnection */
			("Twitter Set Status"),
			("Error setting Twitter Status"),
			(message));
}

static void twitter_send_dm_error_cb(PurpleAccount *acct, const TwitterRequestErrorData *error_data, gpointer user_data)
{
	const char *message;
	if (error_data->type == TWITTER_REQUEST_ERROR_SERVER || error_data->type == TWITTER_REQUEST_ERROR_TWITTER_GENERAL)
	{
		message = error_data->message;
	} else if (error_data->type == TWITTER_REQUEST_ERROR_INVALID_XML) {
		message = "Unknown reply by twitter server";
	} else {
		message = "Unknown error";
	}
	purple_notify_error(NULL,  /* plugin handle or PurpleConnection */
			("Twitter Send Direct Message"),
			("Error sending Direct Message"),
			(message));
}

static void twitter_send_dm_cb(PurpleAccount *account, xmlnode *node, gpointer user_data)
{
	//TODO: verify dm was sent
	return;
}
static void twitter_send_im_cb(PurpleAccount *account, xmlnode *node, gpointer user_data)
{
	//TODO: verify status was sent
	return;
	/*
	   TwitterStatusData *s = twitter_status_node_parse(node);
	   xmlnode *user_node = xmlnode_get_child(node, "user");
	   TwitterUserData *u = twitter_user_node_parse(user_node);

	   if (!s)
	   return;

	   if (!u)
	   {
	   twitter_status_data_free(s);
	   return;
	   }

	   twitter_buddy_set_status_data(account, u->screen_name, s, FALSE);
	   twitter_user_data_free(u);
	 */
}

static int twitter_send_dm(PurpleConnection *gc, const char *who,
		const char *message, PurpleMessageFlags flags)
{
	if (strlen(message) > MAX_TWEET_LENGTH)
	{
		purple_conv_present_error(who, purple_connection_get_account(gc), "Message is too long");
		return 0;
	}
	else
	{
		//TODO handle errors
		twitter_api_send_dm(purple_connection_get_account(gc),
				who, message, 
				twitter_send_dm_cb, twitter_send_dm_error_cb,
				NULL);
		return 1;
	}
}
static int twitter_send_im(PurpleConnection *gc, const char *who,
		const char *message, PurpleMessageFlags flags)
{
	/* TODO should truncate it rather than drop it????? */
	if (strlen(who) + strlen(message) + 2 > MAX_TWEET_LENGTH)
	{
		purple_conv_present_error(who, purple_connection_get_account(gc), "Message is too long");
		return 0;
	}
	else
	{
		TwitterConnectionData *twitter = gc->proto_data;
		long long in_reply_to_status_id = 0;
		const gchar *reply_id;
		char *status;

		status = g_strdup_printf("@%s %s", who, message);
		reply_id = (const gchar *)g_hash_table_lookup (
				twitter->user_reply_id_table, who);
		if (reply_id)
			in_reply_to_status_id = strtoll (reply_id, NULL, 10);

		//TODO handle errors
		twitter_api_set_status(purple_connection_get_account(gc),
				status, in_reply_to_status_id, twitter_send_im_cb,
				twitter_set_status_error_cb, NULL);
		g_free(status);
		return 1;
	}
	//  const char *from_username = gc->account->username;
	//  PurpleMessageFlags receive_flags = ((flags & ~PURPLE_MESSAGE_SEND)
	//				      | PURPLE_MESSAGE_RECV);
	//  PurpleAccount *to_acct = purple_accounts_find(who, TWITTER_PROTOCOL_ID);
	//  PurpleConnection *to;
	//
	//  purple_debug_info(TWITTER_PROTOCOL_ID, "sending message from %s to %s: %s\n",
	//		    from_username, who, message);
	//
	//  /* is the sender blocked by the recipient's privacy settings? */
	//  if (to_acct && !purple_privacy_check(to_acct, gc->account->username)) {
	//    char *msg = g_strdup_printf(
	//      _("Your message was blocked by %s's privacy settings."), who);
	//    purple_debug_info(TWITTER_PROTOCOL_ID,
	//		      "discarding; %s is blocked by %s's privacy settings\n",
	//		      from_username, who);
	//    purple_conv_present_error(who, gc->account, msg);
	//    g_free(msg);
	//    return 0;
	//  }
	//
	//  /* is the recipient online? */
	//  to = get_twitter_gc(who);
	//  if (to) {  /* yes, send */
	//  PurpleMessageFlags receive_flags = ((flags & ~PURPLE_MESSAGE_SEND)
	//				      | PURPLE_MESSAGE_RECV);
	//    serv_got_im(to, from_username, message, receive_flags, time(NULL));
	//
	//  } else {  /* nope, store as an offline message */
	//    GOfflineMessage *offline_message;
	//    GList *messages;
	//
	//    purple_debug_info(TWITTER_PROTOCOL_ID,
	//		      "%s is offline, sending as offline message\n", who);
	//    offline_message = g_new0(GOfflineMessage, 1);
	//    offline_message->from = g_strdup(from_username);
	//    offline_message->message = g_strdup(message);
	//    offline_message->mtime = time(NULL);
	//    offline_message->flags = receive_flags;
	//
	//    messages = g_hash_table_lookup(goffline_messages, who);
	//    messages = g_list_append(messages, offline_message);
	//    g_hash_table_insert(goffline_messages, g_strdup(who), messages);
	//  }
	//
	//   return 1;
}

static void twitter_set_info(PurpleConnection *gc, const char *info) {
	purple_debug_info(TWITTER_PROTOCOL_ID, "setting %s's user info to %s\n",
			gc->account->username, info);
}

static void twitter_get_info(PurpleConnection *gc, const char *username) {
	//TODO: error check
	//TODO: fix for buddy not on list?
	PurpleNotifyUserInfo *info = purple_notify_user_info_new();
	PurpleBuddy *b = purple_find_buddy(purple_connection_get_account(gc), username);

	if (b)
	{
		TwitterBuddyData *data = twitter_buddy_get_buddy_data(b);
		if (data)
		{
			TwitterUserData *user_data = data->user;
			TwitterStatusData *status_data = data->status;


			if (user_data)
			{
				purple_notify_user_info_add_pair(info, "Description:", user_data->description);
			}
			if (status_data)
			{
				purple_notify_user_info_add_pair(info, "Status:", status_data->text);
			}

			/* show a buddy's user info in a nice dialog box */
			purple_notify_userinfo(gc,	/* connection the buddy info came through */
					username,  /* buddy's username */
					info,      /* body */
					NULL,      /* callback called when dialog closed */
					NULL);     /* userdata for callback */
			return;
		}
	}
	purple_notify_user_info_add_pair(info, "Description:", "No user info");
	purple_notify_userinfo(gc,
		username,
		info,
		NULL,
		NULL);

}


static void twitter_set_status(PurpleAccount *acct, PurpleStatus *status) {
	gboolean sync_status = purple_account_get_bool (
			acct, TWITTER_PREF_SYNC_STATUS,
			TWITTER_PREF_SYNC_STATUS_DEFAULT);
	if (!sync_status)
		return ;

	const char *msg = purple_status_get_attr_string(status, "message");
	purple_debug_info(TWITTER_PROTOCOL_ID, "setting %s's status to %s: %s\n",
			acct->username, purple_status_get_name(status), msg);

	if (msg && strcmp("", msg))
	{
		//TODO, sucecss && fail
		twitter_api_set_status(acct, msg, 0,
				NULL, twitter_set_status_error_cb, NULL);
	}
}

static void twitter_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
		PurpleGroup *group)
{
	//TODO
}

static void twitter_add_buddies(PurpleConnection *gc, GList *buddies,
		GList *groups) {
	GList *buddy = buddies;
	GList *group = groups;

	purple_debug_info(TWITTER_PROTOCOL_ID, "adding multiple buddies\n");

	while (buddy && group) {
		twitter_add_buddy(gc, (PurpleBuddy *)buddy->data, (PurpleGroup *)group->data);
		buddy = g_list_next(buddy);
		group = g_list_next(group);
	}
}

static void twitter_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
		PurpleGroup *group)
{
	TwitterBuddyData *twitter_buddy_data = buddy->proto_data;

	purple_debug_info(TWITTER_PROTOCOL_ID, "removing %s from %s's buddy list\n",
			buddy->name, gc->account->username);

	if (!twitter_buddy_data)
		return;
	twitter_user_data_free(twitter_buddy_data->user);
	twitter_status_data_free(twitter_buddy_data->status);
	g_free(twitter_buddy_data);
	buddy->proto_data = NULL;
}

static void twitter_remove_buddies(PurpleConnection *gc, GList *buddies,
		GList *groups) {
	GList *buddy = buddies;
	GList *group = groups;

	purple_debug_info(TWITTER_PROTOCOL_ID, "removing multiple buddies\n");

	while (buddy && group) {
		twitter_remove_buddy(gc, (PurpleBuddy *)buddy->data,
				(PurpleGroup *)group->data);
		buddy = g_list_next(buddy);
		group = g_list_next(group);
	}
}

static void twitter_get_cb_info(PurpleConnection *gc, int id, const char *who) {
	//TODO FIX ME
	PurpleConversation *conv = purple_find_chat(gc, id);
	purple_debug_info(TWITTER_PROTOCOL_ID,
			"retrieving %s's info for %s in chat room %s\n", who,
			gc->account->username, conv->name);

	twitter_get_info(gc, who);
}

/* normalize a username (e.g. remove whitespace, add default domain, etc.)
 * for twitter, this is a noop.
 */
static const char *twitter_normalize(const PurpleAccount *acct,
		const char *input) {
	return NULL;
}

static void twitter_set_buddy_icon(PurpleConnection *gc,
		PurpleStoredImage *img) {
	purple_debug_info(TWITTER_PROTOCOL_ID, "setting %s's buddy icon to %s\n",
			gc->account->username, purple_imgstore_get_filename(img));
}


/*
 * prpl stuff. see prpl.h for more information.
 */

static PurplePluginProtocolInfo prpl_info =
{
	OPT_PROTO_CHAT_TOPIC,  /* options */
	NULL,	       /* user_splits, initialized in twitter_init() */
	NULL,	       /* protocol_options, initialized in twitter_init() */
	{   /* icon_spec, a PurpleBuddyIconSpec */
		"png,jpg,gif",		   /* format */
		0,			       /* min_width */
		0,			       /* min_height */
		48,			     /* max_width */
		48,			     /* max_height */
		10000,			   /* max_filesize */
		PURPLE_ICON_SCALE_DISPLAY,       /* scale_rules */
	},
	twitter_list_icon,		  /* list_icon */ //TODO
	NULL, //twitter_list_emblem,		/* list_emblem */
	twitter_status_text,		/* status_text */
	twitter_tooltip_text,	       /* tooltip_text */
	twitter_status_types,	       /* status_types */
	NULL, //twitter_blist_node_menu,	    /* blist_node_menu */
	twitter_chat_info,		  /* chat_info */
	twitter_chat_info_defaults,	 /* chat_info_defaults */
	twitter_login,		      /* login */
	twitter_close,		      /* close */
	twitter_send_im, //twitter_send_dm,		    /* send_im */
	twitter_set_info,		   /* set_info */
	NULL, //twitter_send_typing,		/* send_typing */
	twitter_get_info,		   /* get_info */
	twitter_set_status,		 /* set_status */
	NULL,		   /* set_idle */
	NULL,//TODO?	      /* change_passwd */
	twitter_add_buddy,//TODO		  /* add_buddy */
	twitter_add_buddies,//TODO		/* add_buddies */
	twitter_remove_buddy,//TODO	       /* remove_buddy */
	twitter_remove_buddies,//TODO	     /* remove_buddies */
	NULL,//TODO?		 /* add_permit */
	NULL,//TODO?		   /* add_deny */
	NULL,//TODO?		 /* rem_permit */
	NULL,//TODO?		   /* rem_deny */
	NULL,//TODO?	    /* set_permit_deny */
	twitter_chat_join,		  /* join_chat */
	NULL,		/* reject_chat */
	NULL,	      /* get_chat_name */
	NULL,		/* chat_invite */
	twitter_chat_leave,		 /* chat_leave */
	NULL,//twitter_chat_whisper,	       /* chat_whisper */
	twitter_chat_send,		  /* chat_send */
	NULL,//TODO?				/* keepalive */
	NULL,	      /* register_user */
	twitter_get_cb_info,		/* get_cb_info */
	NULL,				/* get_cb_away */
	NULL,//TODO?		/* alias_buddy */
	NULL,		/* group_buddy */
	NULL,	       /* rename_group */
	NULL,				/* buddy_free */
	NULL,	       /* convo_closed */
	twitter_normalize,		  /* normalize */
	twitter_set_buddy_icon,	     /* set_buddy_icon */
	NULL,	       /* remove_group */
	NULL,//TODO?				/* get_cb_real_name */
	NULL,	     /* set_chat_topic */
	NULL,				/* find_blist_chat */
	NULL,	  /* roomlist_get_list */
	NULL,	    /* roomlist_cancel */
	NULL,   /* roomlist_expand_category */
	NULL,	   /* can_receive_file */
	NULL,				/* send_file */
	NULL,				/* new_xfer */
	NULL,	    /* offline_message */
	NULL,				/* whiteboard_prpl_ops */
	NULL,				/* send_raw */
	NULL,				/* roomlist_room_serialize */
	NULL,				/* padding... */
	NULL,
	NULL,
	sizeof(PurplePluginProtocolInfo),    /* struct_size */
	NULL
};

static void twitter_init(PurplePlugin *plugin)
{
	PurpleAccountOption *option;
	purple_debug_info(TWITTER_PROTOCOL_ID, "starting up\n");

	option = purple_account_option_bool_new(
			("Enable HTTPS"),      /* text shown to user */
			"use_https",                         /* pref name */
			FALSE);                        /* default value */
	prpl_info.protocol_options = g_list_append(NULL, option);

	/* Retrieve tweets history after login */
	option = purple_account_option_bool_new (
			("Retrieve tweets history after login"),
			TWITTER_PREF_RETRIEVE_HISTORY,
			TWITTER_PREF_RETRIEVE_HISTORY_DEFAULT);
	prpl_info.protocol_options = g_list_append (prpl_info.protocol_options, option);

	/* Sync presence update to twitter */
	option = purple_account_option_bool_new (
			("Sync availability status message to Twitter"),
			TWITTER_PREF_SYNC_STATUS,
			TWITTER_PREF_SYNC_STATUS_DEFAULT);
	prpl_info.protocol_options = g_list_append (prpl_info.protocol_options, option);

	/* Add URL link to each tweet */
	option = purple_account_option_bool_new (
			("Add URL link to each tweet"),
			TWITTER_PREF_ADD_URL_TO_TWEET,
			TWITTER_PREF_ADD_URL_TO_TWEET_DEFAULT);
	prpl_info.protocol_options = g_list_append (prpl_info.protocol_options, option);

	/* API host URL. twitter.com by default.
	 * Users can change it to a proxy URL
	 * This can fuck GFW (http://en.wikipedia.org/wiki/Golden_Shield_Project) */
	option = purple_account_option_string_new (
			("Host URL"),      /* text shown to user */
			TWITTER_PREF_HOST_URL,                         /* pref name */
			TWITTER_PREF_HOST_URL_DEFAULT);                        /* default value */
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	/* Search API host URL. search.twitter.com by default */
	option = purple_account_option_string_new (
			("Search Host URL"),      /* text shown to user */
			TWITTER_PREF_SEARCH_HOST_URL,                         /* pref name */
			TWITTER_PREF_SEARCH_HOST_URL_DEFAULT);                        /* default value */
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	/* Mentions/replies tweets refresh interval */
	option = purple_account_option_int_new(
			("Refresh replies every (min)"),      /* text shown to user */
			TWITTER_PREF_REPLIES_TIMEOUT,                         /* pref name */
			TWITTER_PREF_REPLIES_TIMEOUT_DEFAULT);                        /* default value */
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	/* Friendlist refresh interval */
	option = purple_account_option_int_new(
			("Refresh friendlist every (min)"),      /* text shown to user */
			TWITTER_PREF_USER_STATUS_TIMEOUT,                         /* pref name */
			TWITTER_PREF_USER_STATUS_TIMEOUT_DEFAULT);                        /* default value */
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	/* Search results refresh interval */
	option = purple_account_option_int_new(
			("Refresh search results every (min)"),      /* text shown to user */
			TWITTER_PREF_SEARCH_TIMEOUT,                         /* pref name */
			TWITTER_PREF_SEARCH_TIMEOUT_DEFAULT);                        /* default value */
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);


	_twitter_protocol = plugin;
}

static void twitter_destroy(PurplePlugin *plugin) {
	purple_debug_info(TWITTER_PROTOCOL_ID, "shutting down\n");
}

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,				     /* magic */
	PURPLE_MAJOR_VERSION,				    /* major_version */
	PURPLE_MINOR_VERSION,				    /* minor_version */
	PURPLE_PLUGIN_PROTOCOL,				  /* type */
	NULL,						    /* ui_requirement */
	0,						       /* flags */
	NULL,						    /* dependencies */
	PURPLE_PRIORITY_DEFAULT,				 /* priority */
	TWITTER_PROTOCOL_ID,					     /* id */
	"Twitter Protocol",					      /* name */
	"0.2",						   /* version */
	"Twitter Protocol Plugin",				  /* summary */
	"Twitter Protocol Plugin",				  /* description */
	"neaveru <neaveru@gmail.com>",		     /* author */
	"http://code.google.com/p/libpurple-twitter-protocol/",  /* homepage */
	NULL,						    /* load */
	NULL,						    /* unload */
	twitter_destroy,					/* destroy */
	NULL,						    /* ui_info */
	&prpl_info,					      /* extra_info */
	NULL,						    /* prefs_info */
	twitter_actions,					/* actions */
	NULL,						    /* padding... */
	NULL,
	NULL,
	NULL,
};

PURPLE_INIT_PLUGIN(null, twitter_init, info);
