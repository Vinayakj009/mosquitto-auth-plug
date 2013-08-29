/*
 * Copyright (c) 2013 Jan-Piet Mens <jpmens()gmail.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mosquitto.h>
#include "be-mysql.h"
#include "log.h"
#include "hash.h"

struct mysql_backend {
        MYSQL *mysql;
	char *host;
	int port;
	char *dbname;
	char *user;
	char *pass;
        char *userquery;        // MUST return 1 row, 1 column
        char *superquery;       // MUST return 1 row, 1 column, [0, 1]
        char *aclquery;         // MAY return n rows, 1 column, string
};

void *be_mysql_init()
{
	struct mysql_backend *conf;
	char *host, *user, *pass, *dbname, *p;
	char *userquery;
	int port;

	_log(LOG_DEBUG, "}}}} MYSQL");

	host		= p_stab("host");
	p		= p_stab("port");
	user		= p_stab("user");
	pass		= p_stab("pass");
	dbname		= p_stab("dbname");

	host = (host) ? host : strdup("localhost");
	port = (!p) ? 3306 : atoi(p);

	userquery = p_stab("userquery");

	if (!userquery) {
		_fatal("Mandatory option 'userquery' is missing");
		return (NULL);
	}

	if ((conf = (struct mysql_backend *)malloc(sizeof(struct mysql_backend))) == NULL)
		return (NULL);

	conf->mysql		= mysql_init(NULL);
	conf->host		= host;
	conf->port		= port;
	conf->user		= user;
	conf->pass		= pass;
	conf->dbname		= dbname;
	conf->userquery		= userquery;
	conf->superquery	= p_stab("superquery");
	conf->aclquery		= p_stab("aclquery");

	if (!mysql_real_connect(conf->mysql, host, user, pass, dbname, port, NULL, 0)) {
		fprintf(stderr, "%s\n", mysql_error(conf->mysql));
		free(conf);
		mysql_close(conf->mysql);
		return (NULL);
	}

	return ((void *)conf);
}

void be_mysql_destroy(void *handle)
{
	struct mysql_backend *conf = (struct mysql_backend *)handle;

	if (conf) {
		mysql_close(conf->mysql);
		if (conf->userquery)
			free(conf->userquery);
		if (conf->superquery)
			free(conf->superquery);
		if (conf->aclquery)
			free(conf->aclquery);
		free(conf);
	}
}

static char *escape(void *handle, const char *value, long *vlen)
{
	struct mysql_backend *conf = (struct mysql_backend *)handle;
	char *v;

	*vlen = strlen(value) * 2 + 1;
	if ((v = malloc(*vlen)) == NULL)
		return (NULL);
	mysql_real_escape_string(conf->mysql, v, value, strlen(value));
	return (v);
}

char *be_mysql_getuser(void *handle, const char *username)
{
	struct mysql_backend *conf = (struct mysql_backend *)handle;
	char *query = NULL, *u = NULL, *value = NULL, *v;
	long nrows, ulen;
	MYSQL_RES *res = NULL;
	MYSQL_ROW rowdata;

	if (!conf || !conf->userquery)
		return (NULL);

	if ((u = escape(conf, username, &ulen)) == NULL)
		return (NULL);

	if ((query = malloc(strlen(conf->userquery) + ulen + 128)) == NULL) {
		free(u);
		return (NULL);
	}
	sprintf(query, conf->userquery, u);
	free(u);

	// DEBUG puts(query);

	if (mysql_query(conf->mysql, query)) {
		fprintf(stderr, "%s\n", mysql_error(conf->mysql));
		goto out;
	}

	res = mysql_store_result(conf->mysql);
	if ((nrows = mysql_num_rows(res)) != 1) {
		// DEBUG fprintf(stderr, "rowcount = %ld; not ok\n", nrows);
		goto out;
	}

	if (mysql_num_fields(res) != 1) {
		// DEBUG fprintf(stderr, "numfields not ok\n");
		goto out;
	}

	if ((rowdata = mysql_fetch_row(res)) == NULL) {
		goto out;
	}

	v = rowdata[0];
	value = (v) ? strdup(v) : NULL;


   out:

	mysql_free_result(res);
	free(query);

	return (value);
}

/*
 * Return T/F if user is superuser
 */

int be_mysql_superuser(void *handle, const char *username)
{
	struct mysql_backend *conf = (struct mysql_backend *)handle;
	char *query = NULL, *u = NULL;
	long nrows, ulen;
	int issuper = FALSE;
	MYSQL_RES *res = NULL;
	MYSQL_ROW rowdata;


	if (!conf || !conf->superquery)
		return (FALSE);

	if ((u = escape(conf, username, &ulen)) == NULL)
		return (FALSE);

	if ((query = malloc(strlen(conf->superquery) + ulen + 128)) == NULL) {
		free(u);
		return (FALSE);
	}
	sprintf(query, conf->superquery, u);
	free(u);

	// puts(query);

	if (mysql_query(conf->mysql, query)) {
		fprintf(stderr, "%s\n", mysql_error(conf->mysql));
		goto out;
	}

	res = mysql_store_result(conf->mysql);
	if ((nrows = mysql_num_rows(res)) != 1) {
		goto out;
	}

	if (mysql_num_fields(res) != 1) {
		// DEBUG fprintf(stderr, "numfields not ok\n");
		goto out;
	}

	if ((rowdata = mysql_fetch_row(res)) == NULL) {
		goto out;
	}

	issuper = atoi(rowdata[0]);

   out:

	mysql_free_result(res);
	free(query);

	return (issuper);
}

/*
 * Check ACL.
 * username is the name of the connected user attempting
 * to access
 * topic is the topic user is trying to access (may contain
 * wildcards)
 * acc is desired type of access: read/write		// FIXME 
 *	for subscriptions (READ) (1)
 *	for publish (WRITE) (2)
 *
 * SELECT topic FROM table WHERE username = '%s' AND acc = %d		// may user SUB or PUB topic?
 * SELECT topic FROM table WHERE username = '%s'              		// ignore ACC
 */

int be_mysql_aclcheck(void *handle, const char *username, const char *topic, int acc)
{
	struct mysql_backend *conf = (struct mysql_backend *)handle;
	char *query = NULL, *u = NULL, *v;
	long ulen;
	int match = 0;
	bool bf;
	MYSQL_RES *res = NULL;
	MYSQL_ROW rowdata;


	if (!conf || !conf->aclquery)
		return (FALSE);

	if ((u = escape(conf, username, &ulen)) == NULL)
		return (FALSE);

	if ((query = malloc(strlen(conf->aclquery) + ulen + 128)) == NULL) {
		free(u);
		return (FALSE);
	}
	sprintf(query, conf->aclquery, u, acc);
	free(u);

	// puts(query);

	if (mysql_query(conf->mysql, query)) {
		_log(LOG_NOTICE, "%s", mysql_error(conf->mysql));
		goto out;
	}

	res = mysql_store_result(conf->mysql);
	if (mysql_num_fields(res) != 1) {
		fprintf(stderr, "numfields not ok\n");
		goto out;
	}

	while (match == 0 && (rowdata = mysql_fetch_row(res)) != NULL) {
		if ((v = rowdata[0]) != NULL) {

			/* Check mosquitto_match_topic. If true,
			 * if true, set match and break out of loop. */

			mosquitto_topic_matches_sub(v, topic, &bf);
			match |= bf;
			_log(LOG_DEBUG, "  mysql: topic_matches(%s, %s) == %d",
				topic, v, bf);
		}
	}

   out:

	mysql_free_result(res);
	free(query);

	return (match);
}
