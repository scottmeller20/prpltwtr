void twitter_api_get_friends(PurpleAccount *account,
		TwitterSendRequestMultiPageAllSuccessFunc success_func,
		TwitterSendRequestMultiPageAllErrorFunc error_func,
		gpointer data);

void twitter_api_get_replies(PurpleAccount *account,
		unsigned int since_id,
		TwitterSendRequestMultiPageSuccessFunc success_func,
		TwitterSendRequestMultiPageErrorFunc error_func,
		gpointer data);

void twitter_api_get_rate_limit_status(PurpleAccount *account,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);

void twitter_api_set_status(PurpleAccount *acct, 
		const char *msg,
		TwitterSendRequestSuccessFunc success_func,
		TwitterSendRequestErrorFunc error_func,
		gpointer data);
