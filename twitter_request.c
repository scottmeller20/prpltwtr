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

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <glib.h>

/* If you're using this as the basis of a prpl that will be distributed
 * separately from libpurple, remove the internal.h include below and replace
 * it with code to include your own config.h or similar.  If you're going to
 * provide for translation, you'll also need to setup the gettext macros. */
//#include "internal.h"

#include "request.h"
#include "twitter_request.h"

typedef struct {
	PurpleAccount *account;
	TwitterSendRequestFunc success_func;
	TwitterSendRequestFunc error_func;
	gpointer user_data;
	char *request_text; //TEMP
	GString *response;
} TwitterSendRequestData;

typedef struct
{
	gpointer user_data;
	char *url;
	char *query_string;
	TwitterSendRequestFunc success_callback;
	TwitterSendRequestFunc error_callback;
	int page;
	int expected_count;
} TwitterMultiPageRequestData;

void twitter_send_request_multipage_do(PurpleAccount *account,
		TwitterMultiPageRequestData *request_data);

void twitter_response_cb(const gchar *url_text, gsize len, const gchar *error_message,
		TwitterSendRequestData *request_data)
{
	if (error_message)
	{
		printf("Request_cb error: %s\n", error_message);
		//TODO: handle
	} else {
		xmlnode *node = xmlnode_from_str(url_text, len);
		printf("Response: %s\n", url_text);
		if (!node)
		{
			//TODO: handle
		} else {
			if (xmlnode_get_child(node, "error"))
			{
				if (request_data->error_func)
				{
					request_data->error_func(request_data->account, node, request_data->user_data);
				}
			} else {
				if (request_data->success_func)
					request_data->success_func(request_data->account, node, request_data->user_data);
			}
			g_free(node);
		}
	}
}
void twitter_send_request_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data,
		const gchar *url_text, gsize len,
		const gchar *error_message)
{
	twitter_response_cb(url_text, len, error_message, (TwitterSendRequestData *) user_data);
	g_free(user_data);
}

const gchar *twitter_send_request_response_content(const gchar *response, int len)
{
	static gchar *content_length_token = "\r\nContent-Length: ";
	gchar *content_length_token_pos = NULL;
	int content_length_token_len = strlen(content_length_token);
	const gchar *content = NULL;

	if ((content_length_token_pos = strstr(response, content_length_token)) != NULL)
	{
		if (len <= (content_length_token_pos - response) + content_length_token_len)
			return NULL;
		int content_length_expected = -1;
		content_length_expected = atoi(content_length_token_pos + content_length_token_len);
		if (content_length_expected == -1)
			return NULL;
		if (content_length_expected == 0 && content_length_token_pos[content_length_token_len] != '0')
			return NULL;

		content = g_strstr_len(response, len, "\r\n\r\n");
		if (content == NULL)
			return NULL;
		content += 4;

		if (len - (content - response) < content_length_expected)
			return NULL;
		return content;
	}
	return NULL;

}

void twitter_send_request_receive_data(gpointer data, PurpleSslConnection *gsc,
		PurpleInputCondition cond)
{
	TwitterSendRequestData *request_data = data;
	const gchar *response_content;
	int len;
	static char buf[4096];

	while((len = purple_ssl_read(gsc, buf, sizeof(buf))) > 0) {
		request_data->response = g_string_append_len(request_data->response, buf, len);
	}

	response_content = twitter_send_request_response_content(request_data->response->str, request_data->response->len);

	if (response_content != NULL)
	{
		twitter_response_cb(response_content, request_data->response->len - (response_content - request_data->response->str),
				NULL,
				request_data);
	} else {
		return;
	}

	g_string_free(request_data->response, TRUE);
	g_free(request_data);

	purple_ssl_close(gsc);
}
void twitter_send_request_connected(gpointer data, PurpleSslConnection *gsc,
		PurpleInputCondition cond)
{
	TwitterSendRequestData *request_data = data;
	purple_ssl_input_add(gsc, twitter_send_request_receive_data, data);
	printf("Writing: %s\n", request_data->request_text);
	purple_ssl_write(gsc, request_data->request_text, strlen(request_data->request_text));

	g_free(request_data->request_text);
	request_data->request_text = NULL;
}


void twitter_send_request(PurpleAccount *account, gboolean post,
		const char *url, const char *query_string, 
		TwitterSendRequestFunc success_callback, TwitterSendRequestFunc error_callback,
		gpointer data)
{
	gchar *request;
	const char *pass = purple_connection_get_password(purple_account_get_connection(account));
	const char *sn = purple_account_get_username(account);
	char *auth_text = g_strdup_printf("%s:%s", sn, pass);
	char *auth_text_b64 = purple_base64_encode((guchar *) auth_text, strlen(auth_text));
	TwitterSendRequestData *request_data = g_new0(TwitterSendRequestData, 1);

	request_data->account = account;
	request_data->user_data = data;
	request_data->success_func = success_callback;
	request_data->error_func = error_callback;

	g_free(auth_text);

	request = g_strdup_printf(
			"%s %s%s%s HTTP/1.0\r\n"
			"User-Agent: Mozilla/4.0 (compatible; MSIE 5.5)\r\n"
			"Host: twitter.com\r\n"
			"Authorization: Basic %s\r\n"
			"Content-Length: %d\r\n\r\n"
			"%s",
			post ? "POST" : "GET",
			url, (!post && query_string ? "?" : ""), (!post && query_string ? query_string : ""),
			auth_text_b64,
			query_string  && post ? strlen(query_string) : 0,
			query_string && post ? query_string : "");

	request_data->request_text = request;
	request_data->response = g_string_sized_new(4096);

	g_free(auth_text_b64);

	purple_ssl_connect(account, "www.twitter.com",
			443, twitter_send_request_connected,
			NULL, request_data);

}

void twitter_send_request_multipage_cb(PurpleAccount *account, xmlnode *node, gpointer user_data)
{
	TwitterMultiPageRequestData *request_data = user_data;
	xmlnode *child = node->child;
	int count = 0;
	while ((child = child->next) != NULL)
		if (child->name)
			count++;

	request_data->success_callback(account, node, request_data->user_data);

	if (count < request_data->expected_count)
	{
		g_free(request_data->url);
		if (request_data->query_string)
			g_free(request_data->query_string);
	} else {
		request_data->page++;
		twitter_send_request_multipage_do(account, request_data);
	}
}

void twitter_send_request_multipage_do(PurpleAccount *account,
		TwitterMultiPageRequestData *request_data)
{
	char *full_query_string = g_strdup_printf("%s%spage=%d",
			request_data->query_string ? request_data->query_string : "",
			request_data->query_string && strlen(request_data->query_string) > 0 ? "&" : "",
			request_data->page);

	twitter_send_request(account, FALSE,
			request_data->url, full_query_string,
			twitter_send_request_multipage_cb, NULL,
			request_data);
	g_free(full_query_string);
}

//don't include count in the query_string
void twitter_send_request_multipage(PurpleAccount *account, 
		const char *url, const char *query_string,
		TwitterSendRequestFunc success_callback, TwitterSendRequestFunc error_callback,
		int expected_count, gpointer data)
{
	TwitterMultiPageRequestData *request_data = g_new0(TwitterMultiPageRequestData, 1);
	request_data->user_data = data;
	request_data->url = g_strdup(url);
	request_data->query_string = query_string ? g_strdup(query_string) : NULL;
	request_data->success_callback = success_callback;
	request_data->error_callback = error_callback;
	request_data->page = 1;
	request_data->expected_count = expected_count;

	twitter_send_request_multipage_do(account, request_data);
}

