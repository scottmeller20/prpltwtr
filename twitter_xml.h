#include "config.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <glib.h>

#include <account.h>
#include <accountopt.h>
#include <blist.h>
#include <cmds.h>
#include <conversation.h>
#include <connection.h>
#include <debug.h>
#include <notify.h>
#include <privacy.h>
#include <prpl.h>
#include <roomlist.h>
#include <status.h>
#include <util.h>
#include <version.h>
#include <cipher.h>
#include <sslconn.h>
#include <request.h>
#include "twitter_util.h"

typedef struct 
{
	PurpleAccount *account;
	long long id;
	char *name;
	char *screen_name;
	char *profile_image_url;
	char *description;
} TwitterUserData;

typedef struct 
{
	char *text;
	long long id;
	long long in_reply_to_status_id;
	char *in_reply_to_screen_name;
	time_t created_at;
} TwitterStatusData;

typedef struct
{
	TwitterStatusData *status;
	TwitterUserData *user;
} TwitterBuddyData;

TwitterUserData *twitter_user_node_parse(xmlnode *user_node);
TwitterStatusData *twitter_status_node_parse(xmlnode *status_node);
GList *twitter_users_node_parse(xmlnode *users_node);
GList *twitter_users_nodes_parse(GList *nodes);
GList *twitter_statuses_node_parse(xmlnode *statuses_node);
GList *twitter_statuses_nodes_parse(GList *nodes);