/* Simple HLR/VLR database backend using dbi */
/* (C) 2008 by Jan Luebbe <jluebbe@debian.org>
 * (C) 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 * All Rights Reserved
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <openbsc/gsm_data.h>
#include <openbsc/db.h>

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dbi/dbi.h>

static char *db_basename = NULL;
static char *db_dirname = NULL;
static dbi_conn conn;
static struct gsm_network *current_network = NULL;

static char *create_stmts[] = {
	"CREATE TABLE IF NOT EXISTS Meta ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"key TEXT UNIQUE NOT NULL, "
		"value TEXT NOT NULL"
		")",
	"INSERT OR IGNORE INTO Meta "
		"(key, value) "
		"VALUES "
		"('revision', '1')",
	"CREATE TABLE IF NOT EXISTS Subscriber ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"created TIMESTAMP NOT NULL, "
		"updated TIMESTAMP NOT NULL, "
		"imsi NUMERIC UNIQUE NOT NULL, "
		"name TEXT, "
		"extension TEXT UNIQUE, "
		"authorized INTEGER NOT NULL DEFAULT 0, "
		"tmsi TEXT UNIQUE, "
		"lac INTEGER NOT NULL DEFAULT 0"
		")",
	"CREATE TABLE IF NOT EXISTS Equipment ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"created TIMESTAMP NOT NULL, "
		"updated TIMESTAMP NOT NULL, "
		"name TEXT, "
		"imei NUMERIC UNIQUE NOT NULL"
		")",
	"CREATE TABLE IF NOT EXISTS EquipmentWatch ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"created TIMESTAMP NOT NULL, "
		"updated TIMESTAMP NOT NULL, "
		"subscriber_id NUMERIC NOT NULL, "
		"equipment_id NUMERIC NOT NULL, "
		"UNIQUE (subscriber_id, equipment_id) "
		")",
	"CREATE TABLE IF NOT EXISTS SMS ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"created TIMESTAMP NOT NULL, "
		"sent TIMESTAMP, "
		"sender_id NUMERIC NOT NULL, "
		"receiver_id NUMERIC NOT NULL, "
		"header NUMERIC, "
		"text TEXT NOT NULL "
		")",
};

void db_error_func(dbi_conn conn, void* data) {
	const char* msg;
	dbi_conn_error(conn, &msg);
	printf("DBI: %s\n", msg);
}

int db_init(const char *name, struct gsm_network *network) {
	dbi_initialize(NULL);
	conn = dbi_conn_new("sqlite3");
	if (conn==NULL) {
		printf("DB: Failed to create connection.\n");
		return 1;
	}

	dbi_conn_error_handler( conn, db_error_func, NULL );

	/* MySQL
	dbi_conn_set_option(conn, "host", "localhost");
	dbi_conn_set_option(conn, "username", "your_name");
	dbi_conn_set_option(conn, "password", "your_password");
	dbi_conn_set_option(conn, "dbname", "your_dbname");
	dbi_conn_set_option(conn, "encoding", "UTF-8");
	*/

	/* SqLite 3 */
	db_basename = strdup(name);
	db_dirname = strdup(name);
	dbi_conn_set_option(conn, "sqlite3_dbdir", dirname(db_dirname));
	dbi_conn_set_option(conn, "dbname", basename(db_basename));

	if (dbi_conn_connect(conn) < 0) {
		free(db_dirname);
		free(db_basename);
		db_dirname = db_basename = NULL;
		return 1;
	}

	current_network = network;
	return 0;
}

int db_prepare() {
	dbi_result result;
	int i;

	for (i = 0; i < ARRAY_SIZE(create_stmts); i++) {
		result = dbi_conn_query(conn, create_stmts[i]);
		if (result==NULL) {
			printf("DB: Failed to create some table.\n");
			return 1;
		}
		dbi_result_free(result);
	}

	return 0;
}

int db_fini() {
	dbi_conn_close(conn);
	dbi_shutdown();

	if (db_dirname)
	    free(db_dirname);
	if (db_basename)
	    free(db_basename);
	return 0;
}

struct gsm_subscriber* db_create_subscriber(char *imsi) {
	dbi_result result;
	struct gsm_subscriber* subscr;

	/* Is this subscriber known in the db? */
	subscr = db_get_subscriber(GSM_SUBSCRIBER_IMSI, imsi); 
	if (subscr) {
		result = dbi_conn_queryf(conn,
                         "UPDATE Subscriber set updated = datetime('now') "
                         "WHERE imsi = %s " , imsi);
		if (result==NULL) {
			printf("DB: failed to update timestamp\n");
		} else {
			dbi_result_free(result);
		}
		return subscr;
	}

	subscr = subscr_alloc();
	if (!subscr)
		return NULL;
	result = dbi_conn_queryf(conn,
		"INSERT INTO Subscriber "
		"(imsi, created, updated) "
		"VALUES "
		"(%s, datetime('now'), datetime('now')) ",
		imsi
	);
	if (result==NULL) {
		printf("DB: Failed to create Subscriber by IMSI.\n");
	}
	subscr->id = dbi_conn_sequence_last(conn, NULL);
	strncpy(subscr->imsi, imsi, GSM_IMSI_LENGTH-1);
	dbi_result_free(result);
	printf("DB: New Subscriber: ID %llu, IMSI %s\n", subscr->id, subscr->imsi);
	return subscr;
}

struct gsm_subscriber *db_get_subscriber(enum gsm_subscriber_field field, const char *id) {
	dbi_result result;
	const char *string;
	char *quoted;
	struct gsm_subscriber *subscr;

	switch (field) {
	case GSM_SUBSCRIBER_IMSI:
		dbi_conn_quote_string_copy(conn, id, &quoted);
		result = dbi_conn_queryf(conn,
			"SELECT * FROM Subscriber "
			"WHERE imsi = %s ",
			quoted
		);
		free(quoted);
		break;
	case GSM_SUBSCRIBER_TMSI:
		dbi_conn_quote_string_copy(conn, id, &quoted);
		result = dbi_conn_queryf(conn,
			"SELECT * FROM Subscriber "
			"WHERE tmsi = %s ",
			quoted
		);
		free(quoted);
		break;
	case GSM_SUBSCRIBER_EXTENSION:
		dbi_conn_quote_string_copy(conn, id, &quoted);
		result = dbi_conn_queryf(conn,
			"SELECT * FROM Subscriber "
			"WHERE extension = %s ",
			quoted
		);
		free(quoted);
		break;
	default:
		printf("DB: Unknown query selector for Subscriber.\n");
		return NULL;
	}
	if (result==NULL) {
		printf("DB: Failed to query Subscriber.\n");
		return NULL;
	}
	if (!dbi_result_next_row(result)) {
		printf("DB: Failed to find the Subscriber. '%u' '%s'\n",
			field, id);
		dbi_result_free(result);
		return NULL;
	}

	subscr = subscr_alloc();
	subscr->id = dbi_result_get_ulonglong(result, "id");
	string = dbi_result_get_string(result, "imsi");
	if (string)
		strncpy(subscr->imsi, string, GSM_IMSI_LENGTH);

	string = dbi_result_get_string(result, "tmsi");
	if (string)
		strncpy(subscr->tmsi, string, GSM_TMSI_LENGTH);

	string = dbi_result_get_string(result, "name");
	if (string)
		strncpy(subscr->name, string, GSM_NAME_LENGTH);

	string = dbi_result_get_string(result, "extension");
	if (string)
		strncpy(subscr->extension, string, GSM_EXTENSION_LENGTH);

	subscr->lac = dbi_result_get_uint(result, "lac");
	subscr->authorized = dbi_result_get_uint(result, "authorized");
	printf("DB: Found Subscriber: ID %llu, IMSI %s, NAME '%s', TMSI %s, LAC %hu, AUTH %u\n",
		subscr->id, subscr->imsi, subscr->name, subscr->tmsi,
		subscr->lac, subscr->authorized);
	dbi_result_free(result);
	return subscr;
}

int db_sync_subscriber(struct gsm_subscriber* subscriber) {
	dbi_result result;
	result = dbi_conn_queryf(conn,
		"UPDATE Subscriber "
		"SET updated = datetime('now'), "
		"tmsi = %s, "
		"lac = %i, "
		"authorized = %i "
		"WHERE imsi = %s ",
		subscriber->tmsi, subscriber->lac, subscriber->authorized, subscriber->imsi
	);

	if (result==NULL) {
		printf("DB: Failed to update Subscriber (by IMSI).\n");
		return 1;
	}
	dbi_result_free(result);
	return 0;
}

int db_subscriber_alloc_tmsi(struct gsm_subscriber* subscriber) {
	dbi_result result=NULL;
	char* tmsi_quoted;
	for (;;) {
		sprintf(subscriber->tmsi, "%i", rand());
		dbi_conn_quote_string_copy(conn, subscriber->tmsi, &tmsi_quoted);
		result = dbi_conn_queryf(conn,
			"SELECT * FROM Subscriber "
			"WHERE tmsi = %s ",
			tmsi_quoted
		);
		free(tmsi_quoted);
		if (result==NULL) {
			printf("DB: Failed to query Subscriber while allocating new TMSI.\n");
			return 1;
		}
		if (dbi_result_get_numrows(result)){
			dbi_result_free(result);
			continue;
		}
		if (!dbi_result_next_row(result)) {
			dbi_result_free(result);
			printf("DB: Allocated TMSI %s for IMSI %s.\n", subscriber->tmsi, subscriber->imsi);
			return db_sync_subscriber(subscriber);
		}
		dbi_result_free(result);
	}
	return 0;
}

int db_subscriber_assoc_imei(struct gsm_subscriber* subscriber, char imei[GSM_IMEI_LENGTH]) {
	u_int64_t equipment_id, watch_id;
	dbi_result result;

	result = dbi_conn_queryf(conn,
		"INSERT OR IGNORE INTO Equipment "
		"(imei, created, updated) "
		"VALUES "
		"(%s, datetime('now'), datetime('now')) ",
		imei
	);
	if (result==NULL) {
		printf("DB: Failed to create Equipment by IMEI.\n");
		return 1;
	}
	equipment_id = 0;
	if (dbi_result_get_numrows_affected(result)) {
		equipment_id = dbi_conn_sequence_last(conn, NULL);
	}
	dbi_result_free(result);
	if (equipment_id) {
		printf("DB: New Equipment: ID %llu, IMEI %s\n", equipment_id, imei);
	}
	else {
		result = dbi_conn_queryf(conn,
			"SELECT id FROM Equipment "
			"WHERE imei = %s ",
			imei
		);
		if (result==NULL) {
			printf("DB: Failed to query Equipment by IMEI.\n");
			return 1;
		}
		if (!dbi_result_next_row(result)) {
			printf("DB: Failed to find the Equipment.\n");
			dbi_result_free(result);
			return 1;
		}
		equipment_id = dbi_result_get_ulonglong(result, "id");
		dbi_result_free(result);
	}

	result = dbi_conn_queryf(conn,
		"INSERT OR IGNORE INTO EquipmentWatch "
		"(subscriber_id, equipment_id, created, updated) "
		"VALUES "
		"(%llu, %llu, datetime('now'), datetime('now')) ",
		subscriber->id, equipment_id
	);
	if (result==NULL) {
		printf("DB: Failed to create EquipmentWatch.\n");
		return 1;
	}
	watch_id = 0;
	if (dbi_result_get_numrows_affected(result)) {
		watch_id = dbi_conn_sequence_last(conn, NULL);
	}
	dbi_result_free(result);
	if (watch_id) {
		printf("DB: New EquipmentWatch: ID %llu, IMSI %s, IMEI %s\n", equipment_id, subscriber->imsi, imei);
	}
	else {
		result = dbi_conn_queryf(conn,
			"UPDATE EquipmentWatch "
			"SET updated = datetime('now') "
			"WHERE subscriber_id = %llu AND equipment_id = %llu ",
			subscriber->id, equipment_id
		);
		if (result==NULL) {
			printf("DB: Failed to update EquipmentWatch.\n");
			return 1;
		}
		dbi_result_free(result);
		printf("DB: Updated EquipmentWatch: ID %llu, IMSI %s, IMEI %s\n", equipment_id, subscriber->imsi, imei);
	}

	return 0;
}

/* store an [unsent] SMS to the database */
int db_sms_store(struct gsm_sms *sms)
{
	dbi_result result;
	char *q_text;

	dbi_conn_quote_string_copy(conn, (char *)sms->text, &q_text);
	result = dbi_conn_queryf(conn,
		"INSERT INTO SMS "
		"(created,sender_id,receiver_id,header,text) VALUES "
		"(datetime('now'),%llu,%llu,%s,%s)",
		sms->sender->id,
		sms->receiver ? sms->receiver->id : 0,
		NULL, q_text);
	free(q_text);

	if (!result)
		return -EIO;

	dbi_result_free(result);
	return 0;
}

/* retrieve the next unsent SMS with ID >= min_id */
struct gsm_sms *db_sms_get_unsent(int min_id)
{
	dbi_result result;
	struct gsm_sms *sms = malloc(sizeof(*sms));

	if (!sms) {
		free(sms);
		return NULL;
	}

	result = dbi_conn_queryf(conn,
		"SELECT * FROM SMS "
		"WHERE id >= %llu ORDER BY id", min_id);
	if (!result) {
		free(sms);
		return NULL;
	}

	/* FIXME: fill gsm_sms from database */

	dbi_result_free(result);
	return sms;
}

/* mark a given SMS as read */
int db_sms_mark_sent(struct gsm_sms *sms)
{
	dbi_result result;

	result = dbi_conn_queryf(conn,
		"UPDATE SMS "
		"SET sent = datetime('now') "
		"WHERE id = %llu", sms->id);
	if (!result) {
		printf("DB: Failed to mark SMS %llu as sent.\n", sms->id);
		return 1;
	}

	dbi_result_free(result);
	return 0;
}
