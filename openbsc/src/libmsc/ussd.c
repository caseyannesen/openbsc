/* Network-specific handling of mobile-originated USSDs. */

/* (C) 2008-2009 by Harald Welte <laforge@gnumonks.org>
 * (C) 2008, 2009 by Holger Hans Peter Freyther <zecke@selfish.org>
 * (C) 2009 by Mike Haben <michael.haben@btinternet.com>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/* This module defines the network-specific handling of mobile-originated
   USSD messages. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h> //for usleep

#include <openbsc/gsm_04_80.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/debug.h>
#include <openbsc/osmo_msc.h>

/* Declarations of USSD strings to be recognised */
const char USSD_TEXT_OWN_NUMBER[] = "*1000#";
const char USSD_TERMINATION_ID[] = "0";

//make a custom network request
bool make_ussd_sock_req(char *sck_payload, char *response_string) {
    int sockfd;
    struct sockaddr_in server_addr;

	// Create a socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        DEBUGP(DMM, "Socket Creation Failed!\n");
		return false;
    }

	// Set up the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
         DEBUGP(DMM, "Invalid Address!\n");
		 return false;
    }

	// Connect to the server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        DEBUGP(DMM, "Connection Failed!\n");
		return false;
    }

	// Send a request message to the server
    if (send(sockfd, sck_payload, strlen(sck_payload), 0) == -1) {
        DEBUGP(DMM, "Sending Failed!\n");
		return false;
    }
	DEBUGP(DMM, "Send paylod \'%s\'!\n", sck_payload);

	// Receive a response message from the server
	if (recv(sockfd, response_string, 131, 0) == -1) {
		DEBUGP(DMM, "Receiving Failed!\n");
	}else{
		DEBUGP(DMM, "Received response \'%s\'!\n", response_string);
	}
	// Close the socket gracefully if possible
	if (close(sockfd) == -1) {
		DEBUGP(DMM, "Closing Failed!\n");
	}
    return true;
}

/* A network-specific handler function */
static int send_own_number(struct gsm_subscriber_connection *conn, const struct msgb *msg, const struct ss_request *req)
{
	char *own_number = conn->subscr->extension;
	char response_string[GSM_EXTENSION_LENGTH + 50];

	/* Need trailing CR as EOT character */
	snprintf(response_string, sizeof(response_string), "You extention is %s.", own_number);
	return gsm0480_send_ussd_response(conn, msg, response_string, req);
}

/* Another network-specific handler function */
static int socket_ussd_handler(const struct gsm48_hdr *hdr, struct gsm_subscriber_connection *conn, const struct msgb *msg, const struct ss_request *req)
{
	char *own_number = conn->subscr->imsi;  //typicaly return 0x3A, 0x7A or 0x7B
	char *request_string[120];
	char *response_string[131];
	char *ussd_text = req->ussd_text;
	
	snprintf(request_string, sizeof(request_string), "{\"type\":\"ussd\",\"text\":\"%s\",\"opcode\":\"%d\",\"imsi\":\"%s\"}", ussd_text, ussd_text[0], own_number);

	make_ussd_sock_req(request_string, response_string);
	// Need trailing CR as EOT character 
	return gsm0480_send_ussd_response(conn, msg, response_string, req);
}


/* Entrypoint - handler function common to all mobile-originated USSDs */
int handle_rcv_ussd(struct gsm_subscriber_connection *conn, struct msgb *msg)
{
	int rc;
	struct ss_request req;
	struct gsm48_hdr *gh;

	memset(&req, 0, sizeof(req));
	gh = msgb_l3(msg);
	rc = gsm0480_decode_ss_request(gh, msgb_l3len(msg), &req);
	if (!rc) {
		DEBUGP(DMM, "Unhandled SS\n");
		rc = gsm0480_send_ussd_reject(conn, msg, &req);
		msc_release_connection(conn);
		return rc;
	}

	/* Interrogation or releaseComplete? */
	if (req.ussd_text[0] == '\0' || req.ussd_text[0] == 0xFF) {
		if (req.ss_code > 0) {
			/* Assume interrogateSS or modification of it and reject */
			rc = gsm0480_send_ussd_reject(conn, msg, &req);
			msc_release_connection(conn);
			return rc;
		}
		/* Still assuming a Release-Complete and returning */
		msc_release_connection(conn);
		return 0;
	}

	if (!strcmp(USSD_TEXT_OWN_NUMBER, (const char *)req.ussd_text)) {
		DEBUGP(DMM, "sent %s\n", req.ussd_text);
		rc = send_own_number(conn, msg, &req);
	} 
	else {
		DEBUGP(DMM, "sent %s\n", req.ussd_text);
		DEBUGP(DMM, "ussd_text_code: %d\n", req.ussd_text[0]);
		rc = socket_ussd_handler(gh, conn, msg, &req);
	}

	/* check if we can release it */
	//msc_release_connection(conn);
	return rc;
}
