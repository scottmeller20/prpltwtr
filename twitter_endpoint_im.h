#ifndef _TWITTER_ENDPOINT_IM_H_
#define _TWITTER_ENDPOINT_IM_H_

#include "config.h"

#include "twitter_api.h"

typedef enum
{
	TWITTER_IM_TYPE_AT_MSG = 0,
	TWITTER_IM_TYPE_DM = 1,
	TWITTER_IM_TYPE_UNKNOWN = 2,
} TwitterImType;

typedef void (*TwitterApiImAllFunc) (PurpleAccount *account,
		long long since_id,
		TwitterSendRequestMultiPageAllSuccessFunc success_func,
		TwitterSendRequestMultiPageAllErrorFunc error_func,
		gpointer data);

typedef struct
{
	const gchar *since_id_setting_id;
	int (*timespan_func)(PurpleAccount *account);
	TwitterApiImAllFunc get_im_func;
	TwitterSendRequestMultiPageAllSuccessFunc success_cb;
	TwitterSendRequestMultiPageAllErrorFunc error_cb;
} TwitterEndpointImSettings;

typedef struct
{
	PurpleAccount *account;
	guint timer;
	long long since_id;
	TwitterEndpointImSettings *settings;
} TwitterEndpointIm;

TwitterEndpointIm *twitter_endpoint_im_new(PurpleAccount *account, TwitterEndpointImSettings *settings);
void twitter_endpoint_im_free(TwitterEndpointIm *ctx);

void twitter_endpoint_im_settings_save_since_id(PurpleAccount *account, TwitterEndpointImSettings *settings, long long since_id);
long long twitter_endpoint_im_settings_load_since_id(PurpleAccount *account, TwitterEndpointImSettings *settings);
void twitter_endpoint_im_set_since_id(TwitterEndpointIm *ctx, long long since_id);
long long twitter_endpoint_im_get_since_id(TwitterEndpointIm *ctx);

void twitter_endpoint_im_start(TwitterEndpointIm *ctx);

#endif