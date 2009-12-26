
void twitter_api_get_friends(PurpleAccount *account,
		TwitterSendRequestMultiPageAllSuccessFunc success_func,
		TwitterSendRequestMultiPageAllErrorFunc error_func,
		gpointer data);

void twitter_api_get_replies_all(PurpleAccount *account,
		long long since_id,
		TwitterSendRequestMultiPageAllSuccessFunc success_func,
		TwitterSendRequestMultiPageAllErrorFunc error_func,
		gpointer data);

void twitter_api_get_replies(PurpleAccount *account,
		long long since_id,
		int count,
		int page,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);

void twitter_api_get_rate_limit_status(PurpleAccount *account,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);

void twitter_api_send_dm(PurpleAccount *acct,
		const char *user,
		const char *msg,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);
void twitter_api_set_status(PurpleAccount *acct,
		const char *msg,
		long long in_reply_to_status_id,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);

void twitter_api_get_saved_searches (PurpleAccount *account,
        TwitterSendRequestSuccessFunc success_func,
        TwitterSendRequestErrorFunc error_func,
        gpointer data);

/**
 * @rpp: The number of tweets to return per page, up to a max of 100
 */
void twitter_api_search (PurpleAccount *account,
        const char *keyword,
        long long since_id,
        uint rpp,
        TwitterSearchSuccessFunc success_func,
        TwitterSearchErrorFunc error_func,
        gpointer data);

void twitter_api_search_refresh (PurpleAccount *account,
        const char *refresh_url,
        TwitterSearchSuccessFunc success_func,
        TwitterSearchErrorFunc error_func,
        gpointer data);
