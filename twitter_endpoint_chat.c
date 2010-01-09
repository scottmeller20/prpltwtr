#include "twitter_endpoint_chat.h"

void twitter_endpoint_chat_free(TwitterEndpointChat *ctx)
{
	PurpleConnection *gc;

	if (ctx->settings && ctx->settings->endpoint_data_free)
		ctx->settings->endpoint_data_free(ctx->endpoint_data);
	gc = purple_account_get_connection(ctx->account);

	if (ctx->timer_handle)
	{
		purple_timeout_remove(ctx->timer_handle);
		ctx->timer_handle = 0;
	}
	if (ctx->chat_name)
	{
		g_free(ctx->chat_name);
		ctx->chat_name = NULL;
	}
	g_slice_free(TwitterEndpointChat, ctx);
}

TwitterEndpointChat *twitter_endpoint_chat_new(
	TwitterEndpointChatSettings *settings,
	TwitterChatType type, PurpleAccount *account, const gchar *chat_name,
	GHashTable *components)
{
	TwitterEndpointChat *ctx = g_slice_new0(TwitterEndpointChat);
	ctx->settings = settings;
	ctx->type = type;
	ctx->account = account;
	ctx->chat_name = g_strdup(chat_name);
	ctx->endpoint_data = settings->create_endpoint_data ?
		settings->create_endpoint_data(components) :
		NULL;

	return ctx;
}

PurpleConversation *twitter_chat_context_find_conv(TwitterEndpointChat *ctx)
{
	return purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, ctx->chat_name, ctx->account);
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

PurpleChat *twitter_blist_chat_find_search(PurpleAccount *account, const char *name)
{
	return _twitter_blist_chat_find(account, TWITTER_CHAT_SEARCH, "search", name);
}
PurpleChat *twitter_blist_chat_find_timeline(PurpleAccount *account, gint timeline_id)
{
	char *tmp = g_strdup_printf("%d", timeline_id);
	PurpleChat *chat = _twitter_blist_chat_find(account, TWITTER_CHAT_TIMELINE, "timeline_id", tmp);
	g_free(tmp);
	return chat;
}

//TODO: fix me
PurpleChat *twitter_find_blist_chat(PurpleAccount *account, const char *name)
{
	static char *timeline = "Timeline: ";
	static char *search = "Search: ";
	PurpleChat *c = NULL;
	if (strlen(name) > strlen(timeline) && !strncmp(timeline, name, strlen(timeline)))
	{
		c = twitter_blist_chat_find_timeline(account, 0);
	} else if (strlen(name) > strlen(search) && !strncmp(search, name, strlen(search))) {
		c = twitter_blist_chat_find_search(account, name + strlen(search));
	} else {
		c = twitter_blist_chat_find_search(account, name);
	}
	return c;
}

//TODO should be static?
gint twitter_get_next_chat_id()
{
	static gint chat_id = 1;
	return chat_id++;
}

//TODO should be static?
gboolean twitter_chat_auto_open(PurpleChat *chat)
{
	g_return_val_if_fail(chat != NULL, FALSE);
	GHashTable *components = purple_chat_get_components(chat);
	char *auto_open = g_hash_table_lookup(components, "auto_open");
	return (auto_open != NULL && auto_open[0] != '0');
}

