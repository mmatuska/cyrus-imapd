/*
 * Copyright (c) 1994-2012 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: Sébastien Michel from Atos Worldline
 */
#include <config.h>
#ifdef ENABLE_MBOXEVENT
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <jansson.h>

#include "annotate.h"
#include "assert.h"
#include "exitcodes.h"
#include "imapurl.h"
#include "libconfig.h"
#include "map.h"
#include "times.h"
#include "xmalloc.h"

#include "imap/mboxevent.h"
#include "imap/mboxname.h"
#include "imap/notify.h"


#define MESSAGE_EVENTS (EVENT_MESSAGE_APPEND|EVENT_MESSAGE_EXPIRE|\
			EVENT_MESSAGE_EXPUNGE|EVENT_MESSAGE_NEW|\
			EVENT_MESSAGE_COPY|EVENT_MESSAGE_MOVE)

#define FLAGS_EVENTS   (EVENT_FLAGS_SET|EVENT_FLAGS_CLEAR|EVENT_MESSAGE_READ|\
			EVENT_MESSAGE_TRASH)

#define MAILBOX_EVENTS (EVENT_MAILBOX_CREATE|EVENT_MAILBOX_DELETE|\
			EVENT_MAILBOX_RENAME|EVENT_ACL_CHANGE)

#define SUBS_EVENTS    (EVENT_MAILBOX_SUBSCRIBE|EVENT_MAILBOX_UNSUBSCRIBE)

#define QUOTA_EVENTS   (EVENT_QUOTA_EXCEED|EVENT_QUOTA_WITHIN|EVENT_QUOTA_CHANGE)


#define FILL_STRING_PARAM(e,p,v) e->params[p].value = (uint64_t)v; \
				 e->params[p].type = EVENT_PARAM_STRING; \
				 e->params[p].filled = 1
#define FILL_ARRAY_PARAM(e,p,v) e->params[p].value = (uint64_t)v; \
				 e->params[p].type = EVENT_PARAM_ARRAY; \
				 e->params[p].filled = 1
#define FILL_UNSIGNED_PARAM(e,p,v) e->params[p].value = (uint64_t)v; \
				  e->params[p].type = EVENT_PARAM_INT; \
				  e->params[p].filled = 1

static const char *notifier = NULL;
static struct namespace namespace;

static strarray_t *excluded_flags;
static strarray_t *excluded_specialuse;
static int enable_subfolder = 1;

static int enabled_events = 0;
static unsigned long extra_params;

static struct mboxevent event_template =
{ 0,
  /* ordered to optimize the parsing of the notification message */
  { { EVENT_TIMESTAMP, "timestamp", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_SERVICE, "service", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_SERVER_ADDRESS, "serverAddress", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_CLIENT_ADDRESS, "clientAddress", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_OLD_MAILBOX_ID, "oldMailboxID", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_OLD_UIDSET, "vnd.cmu.oldUidset", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MAILBOX_ID, "mailboxID", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_URI, "uri", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MODSEQ, "modseq", EVENT_PARAM_INT, 0, 0 },
    { EVENT_DISK_QUOTA, "diskQuota", EVENT_PARAM_INT, 0, 0 },
    { EVENT_DISK_USED, "diskUsed", EVENT_PARAM_INT, 0, 0 },
    { EVENT_MAX_MESSAGES, "maxMessages", EVENT_PARAM_INT, 0, 0 },
    { EVENT_ACL_SUBJECT, "aclSubject", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_ACL_RIGHTS, "aclRights", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MESSAGES, "messages", EVENT_PARAM_INT, 0, 0 },
    { EVENT_UNSEEN_MESSAGES, "vnd.cmu.unseenMessages", EVENT_PARAM_INT, 0, 0 },
    { EVENT_UIDNEXT, "uidnext", EVENT_PARAM_INT, 0, 0 },
    { EVENT_UIDSET, "uidset", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MIDSET, "vnd.cmu.midset", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_FLAG_NAMES, "flagNames", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_PID, "pid", EVENT_PARAM_INT, 0, 0 },
    { EVENT_USER, "user", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MESSAGE_SIZE, "messageSize", EVENT_PARAM_INT, 0, 0 },
    /* always at end to let the parser to easily truncate this part */
    { EVENT_ENVELOPE, "vnd.cmu.envelope", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_BODYSTRUCTURE, "bodyStructure", EVENT_PARAM_STRING, 0, 0 },
    { EVENT_MESSAGE_CONTENT, "messageContent", EVENT_PARAM_STRING, 0, 0 }
  },
  STRARRAY_INITIALIZER, { 0, 0 }, NULL, STRARRAY_INITIALIZER, NULL, NULL, NULL
};

static char *json_formatter(enum event_type type, struct event_parameter params[]);
#ifndef NDEBUG
static int filled_params(enum event_type type, struct mboxevent *mboxevent);
#endif
static int mboxevent_expected_param(enum event_type type, enum event_param param);

EXPORTED void mboxevent_init(void)
{
    const char *options;
    int groups;

    if (!(notifier = config_getstring(IMAPOPT_EVENT_NOTIFIER)))
	return;

    /* some don't want to notify events for some IMAP flags */
    options = config_getstring(IMAPOPT_EVENT_EXCLUDE_FLAGS);
    excluded_flags = strarray_split(options, NULL, 0);

    /* some don't want to notify events on some folders (ie. Sent, Spam) */
    /* identify those folders with IMAP SPECIAL-USE */
    options = config_getstring(IMAPOPT_EVENT_EXCLUDE_SPECIALUSE);
    excluded_specialuse = strarray_split(options, NULL, 0);

    /* special meaning to disable event notification on all sub folders */
    if (strarray_find_case(excluded_specialuse, "ALL", 0) >= 0)
	enable_subfolder = 0;

    /* get event types's extra parameters */
    extra_params = config_getbitfield(IMAPOPT_EVENT_EXTRA_PARAMS);

    /* groups of related events to turn on notification */
    groups = config_getbitfield(IMAPOPT_EVENT_GROUPS);
    if (groups & IMAP_ENUM_EVENT_GROUPS_MESSAGE)
	enabled_events |= MESSAGE_EVENTS;

    if (groups & IMAP_ENUM_EVENT_GROUPS_QUOTA)
	enabled_events |= QUOTA_EVENTS;

    if (groups & IMAP_ENUM_EVENT_GROUPS_FLAGS)
	enabled_events |= FLAGS_EVENTS;

    if (groups & IMAP_ENUM_EVENT_GROUPS_ACCESS)
	enabled_events |= (EVENT_LOGIN|EVENT_LOGOUT|EVENT_ACL_CHANGE);

    if (groups & IMAP_ENUM_EVENT_GROUPS_SUBSCRIPTION)
	enabled_events |= SUBS_EVENTS;

    if (groups & IMAP_ENUM_EVENT_GROUPS_MAILBOX)
	enabled_events |= MAILBOX_EVENTS;
}

EXPORTED void mboxevent_setnamespace(struct namespace *n)
{
    namespace = *n;
    /* standardize IMAP URL format */
    namespace.isadmin = 0;
}

static int mboxevent_enabled_for_mailbox(struct mailbox *mailbox)
{
    struct buf attrib = BUF_INITIALIZER;
    strarray_t *specialuse = NULL;
    int enabled = 1;
    int i = 0;
    int r = 0;

    if (!enable_subfolder && (mboxname_isusermailbox(mailbox->name, 1)) == NULL) {
	enabled = 0;
	goto done;
    }

    /* test if the mailbox has a special-use attribute in the exclude list */
    if (strarray_size(excluded_specialuse) > 0) {
	const char *userid = mboxname_to_userid(mailbox->name);

	r = annotatemore_lookup(mailbox->name, "/specialuse", userid, &attrib);
	if (r) goto done; /* XXX - return -1?  Failure? */

	/* get info and set flags */
	specialuse = strarray_split(buf_cstring(&attrib), NULL, 0);

	for (i = 0; i < strarray_size(specialuse) ; i++) {
	    const char *attribute = strarray_nth(specialuse, i);
	    if (strarray_find(excluded_specialuse, attribute, 0) >= 0) {
		enabled = 0;
		goto done;
	    }
	}
    }

done:
    strarray_free(specialuse);
    buf_free(&attrib);
    return enabled;
}

EXPORTED struct mboxevent *mboxevent_new(enum event_type type)
{
    struct mboxevent *mboxevent = NULL;
    /* event notification is completely disabled */
    if (!notifier)
	return NULL;

    /* the group to which belong the event is not enabled */
    if (!(enabled_events & type))
	return NULL;

    mboxevent = xmalloc(sizeof(struct mboxevent));
    memcpy(mboxevent, &event_template, sizeof(struct mboxevent));

    mboxevent->type = type;

    /* From RFC 5423:
     * the time at which the event occurred that triggered the notification
     * (...). This MAY be an approximate time.
     *
     * so it seems appropriate here */
    if (mboxevent_expected_param(type, EVENT_TIMESTAMP))
	gettimeofday(&mboxevent->timestamp, NULL);

    FILL_UNSIGNED_PARAM(mboxevent, EVENT_PID, getpid());

    return mboxevent;
}

struct mboxevent *mboxevent_enqueue(enum event_type type,
				    struct mboxevent **mboxevents)
{
    struct mboxevent *mboxevent = NULL;
    struct mboxevent *ptr;

    if (!(mboxevent = mboxevent_new(type)))
	return NULL;

    if (mboxevents) {
	if (*mboxevents == NULL)
	    *mboxevents = mboxevent;
	else {
	    /* append the newly created event at end of the chained list */
	    ptr = *mboxevents;
	    while (ptr->next)
		ptr = ptr->next;
	    ptr->next = mboxevent;
	    mboxevent->prev = ptr;
	}
    }

    return mboxevent;
}

EXPORTED void mboxevent_free(struct mboxevent **mboxevent)
{
    struct mboxevent *event = *mboxevent;
    int i;

    if (!event)
	return;

    seqset_free(event->uidset);
    seqset_free(event->olduidset);
    strarray_fini(&event->midset);

    for (i = 0; i <= MAX_PARAM; i++) {
	if (event->params[i].filled && event->params[i].type == EVENT_PARAM_STRING)
	    free((char *)event->params[i].value);
    }

    if (event->prev)
	event->prev->next = event->next;

    if (event->next)
	event->next->prev = event->prev;

    free(event);

    *mboxevent = NULL;
}

void mboxevent_freequeue(struct mboxevent **mboxevent)
{
    struct mboxevent *next, *event = *mboxevent;

    if (!event)
	return;

    do {
	next = event->next;
	mboxevent_free(&event);
	event = next;
    }
    while (event);

    *mboxevent = NULL;
}

static int mboxevent_expected_param(enum event_type type, enum event_param param)
{
    switch (param) {
    case EVENT_BODYSTRUCTURE:
	return (extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_BODYSTRUCTURE) &&
	       (type & (EVENT_MESSAGE_NEW|EVENT_MESSAGE_APPEND));
    case EVENT_CLIENT_ADDRESS:
	return (extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_CLIENTADDRESS) &&
	       (type & (EVENT_LOGIN|EVENT_LOGOUT));
    case EVENT_DISK_QUOTA:
	return type & QUOTA_EVENTS;
    case EVENT_DISK_USED:
	return (type & (EVENT_QUOTA_EXCEED|EVENT_QUOTA_WITHIN) ||
		/* quota usage is not known on event MessageNew, MessageAppend,
		 * MessageCopy and MessageExpunge.
		 * Thus, some code refactoring is needed to support diskUsed
		 * extra parameter */
		((extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_DISKUSED) &&
		 (type & (EVENT_QUOTA_CHANGE))));
    case EVENT_ENVELOPE:
	return (extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_VND_CMU_ENVELOPE) &&
	       (type & (EVENT_MESSAGE_NEW|EVENT_MESSAGE_APPEND));
    case EVENT_FLAG_NAMES:
	return (type & (EVENT_FLAGS_SET|EVENT_FLAGS_CLEAR)) ||
	       ((extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_FLAGNAMES) &&
		(type & (EVENT_MESSAGE_APPEND|EVENT_MESSAGE_NEW)));
    case EVENT_MAILBOX_ID:
	return (type & MAILBOX_EVENTS);
    case EVENT_MAX_MESSAGES:
	return type & QUOTA_EVENTS;
    case EVENT_MESSAGE_CONTENT:
	return (extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_MESSAGECONTENT) &&
	       (type & (EVENT_MESSAGE_APPEND|EVENT_MESSAGE_NEW));
    case EVENT_MESSAGE_SIZE:
	return (extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_MESSAGESIZE) &&
	       (type & (EVENT_MESSAGE_APPEND|EVENT_MESSAGE_NEW));
    case EVENT_MESSAGES:
	if (type & (EVENT_QUOTA_EXCEED|EVENT_QUOTA_WITHIN))
	    return 1;
	if (!(extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_MESSAGES))
	    return 0;
	break;
    case EVENT_MODSEQ:
	if (!(extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_MODSEQ))
	    return 0;
	break;
    case EVENT_OLD_MAILBOX_ID:
	return type & (EVENT_MESSAGE_COPY|EVENT_MESSAGE_MOVE|EVENT_MAILBOX_RENAME);
    case EVENT_SERVER_ADDRESS:
	return type & (EVENT_LOGIN|EVENT_LOGOUT);
    case EVENT_SERVICE:
	return extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_SERVICE;
    case EVENT_TIMESTAMP:
	return extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_TIMESTAMP;
    case EVENT_ACL_SUBJECT:
	return type & EVENT_ACL_CHANGE;
    case EVENT_ACL_RIGHTS:
	return type & EVENT_ACL_CHANGE;
    case EVENT_UIDNEXT:
	if (!(extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_UIDNEXT))
	    return 0;
	break;
    case EVENT_UIDSET:
	if (type & (EVENT_MESSAGE_NEW|EVENT_MESSAGE_APPEND))
	    return 0;
	break;
    case EVENT_URI:
	return 1;
    case EVENT_PID:
	return 1;
    case EVENT_USER:
	return type & (EVENT_MAILBOX_SUBSCRIBE|EVENT_MAILBOX_UNSUBSCRIBE|\
		       EVENT_LOGIN|EVENT_LOGOUT);
    case EVENT_MIDSET:
	if (!(extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_VND_CMU_MIDSET))
	    return 0;
	break;
    case EVENT_UNSEEN_MESSAGES:
	if (!(extra_params & IMAP_ENUM_EVENT_EXTRA_PARAMS_VND_CMU_UNSEENMESSAGES))
	    return 0;
	break;
    case EVENT_OLD_UIDSET:
	return type & (EVENT_MESSAGE_COPY|EVENT_MESSAGE_MOVE);
    }

    /* test if the parameter is related to a message event */
    return type & (MESSAGE_EVENTS|FLAGS_EVENTS);
}

#define TIMESTAMP_MAX 32
EXPORTED void mboxevent_notify(struct mboxevent *mboxevents)
{
    enum event_type type;
    struct mboxevent *event, *next;
    char stimestamp[TIMESTAMP_MAX+1];
    char *formatted_message;

    /* nothing to notify */
    if (!mboxevents)
	return;

    event = mboxevents;

    /* swap FlagsSet and FlagsClear notification order depending the presence of
     * the \Seen flag because it changes the value of vnd.cmu.unseenMessages */
    if (event->type == EVENT_FLAGS_SET &&
	event->next &&
	event->next->type == EVENT_FLAGS_CLEAR &&
	strarray_find_case(&event->next->flagnames, "\\Seen", 0) >= 0) {

	next = event->next;
	event->next = next->next;
	next->next = event;
	event = next;
    }

    /* loop over the chained list of events */
    do {
	if (event->type == EVENT_CANCELLED)
	    goto next;

	/* verify that at least one message has been added depending the event type */
	if (event->type & (MESSAGE_EVENTS|FLAGS_EVENTS)) {
	    if (event->type & (EVENT_MESSAGE_NEW|EVENT_MESSAGE_APPEND)) {
		if (!event->params[EVENT_URI].filled)
		    goto next;
	    }
	    else
		if (event->uidset == NULL)
		    goto next;
	}

	/* others quota are not supported by RFC 5423 */
	if ((event->type & QUOTA_EVENTS) &&
	    !event->params[EVENT_DISK_QUOTA].filled &&
	    !event->params[EVENT_MAX_MESSAGES].filled)
	    goto next;

	/* finish to fill event parameters structure */

	if (mboxevent_expected_param(event->type, EVENT_SERVICE)) {
	    FILL_STRING_PARAM(event, EVENT_SERVICE, xstrdup(config_ident));
	}

	if (mboxevent_expected_param(event->type, EVENT_TIMESTAMP)) {
	    timeval_to_iso8601(&event->timestamp, timeval_ms,
			       stimestamp, sizeof(stimestamp));
	    FILL_STRING_PARAM(event, EVENT_TIMESTAMP, xstrdup(stimestamp));
	}

	if (event->uidset) {
	    FILL_STRING_PARAM(event, EVENT_UIDSET, seqset_cstring(event->uidset));
	}
	if (strarray_size(&event->midset) > 0) {
	    FILL_ARRAY_PARAM(event, EVENT_MIDSET, &event->midset);
	}
	if (event->olduidset) {
	    FILL_STRING_PARAM(event, EVENT_OLD_UIDSET, seqset_cstring(event->olduidset));
	}

	/* may split FlagsSet event in several event notifications */
	do {
	    type = event->type;
	    /* prefer MessageRead and MessageTrash to FlagsSet as advised in the RFC */
	    if (type == EVENT_FLAGS_SET) {
		int i;

		if ((i = strarray_find(&event->flagnames, "\\Deleted", 0)) >= 0) {
		    type = EVENT_MESSAGE_TRASH;
		    strarray_remove(&event->flagnames, i);
		}
		else if ((i = strarray_find(&event->flagnames, "\\Seen", 0)) >= 0) {
		    type = EVENT_MESSAGE_READ;
		    strarray_remove(&event->flagnames, i);
		}
	    }

	    if (strarray_size(&event->flagnames) > 0) {
		/* don't send flagNames parameter for those events */
		if (type != EVENT_MESSAGE_TRASH && type != EVENT_MESSAGE_READ) {
		    char *flagnames = strarray_join(&event->flagnames, " ");
		    FILL_STRING_PARAM(event, EVENT_FLAG_NAMES, flagnames);

		    /* stop to loop for flagsSet event here */
		    strarray_fini(&event->flagnames);
		}
	    }

	    /* check if expected event parameters are filled */
	    assert(filled_params(type, event));

	    /* notification is ready to send */
	    formatted_message = json_formatter(type, event->params);
	    notify(notifier, "EVENT", NULL, NULL, NULL, 0, NULL, formatted_message);

	    free(formatted_message);
	}
	while (strarray_size(&event->flagnames) > 0);

    next:
	event = event->next;
    }
    while (event);

    return;
}

void mboxevent_add_flags(struct mboxevent *event, char *flagnames[MAX_USER_FLAGS],
			 bit32 system_flags, bit32 user_flags[MAX_USER_FLAGS/32])
{
    unsigned flag, flagmask = 0;

    if (!event)
	return;

    /* add system flags */
    if (system_flags & FLAG_DELETED) {
	if (strarray_find_case(excluded_flags, "\\Deleted", 0) < 0)
	    strarray_add_case(&event->flagnames, "\\Deleted");
    }
    if (system_flags & FLAG_ANSWERED) {
	if (strarray_find_case(excluded_flags, "\\Answered", 0) < 0)
	    strarray_add_case(&event->flagnames, "\\Answered");
    }
    if (system_flags & FLAG_FLAGGED) {
	if (strarray_find_case(excluded_flags, "\\Flagged", 0) < 0)
	    strarray_add_case(&event->flagnames, "\\Flagged");
    }
    if (system_flags & FLAG_DRAFT) {
	if (strarray_find_case(excluded_flags, "\\Draft", 0) < 0)
	    strarray_add_case(&event->flagnames, "\\Draft");
    }
    if (system_flags & FLAG_SEEN) {
	if (strarray_find_case(excluded_flags, "\\Seen", 0) < 0)
	    strarray_add_case(&event->flagnames, "\\Seen");
    }

    /* add user flags */
    for (flag = 0; flag < MAX_USER_FLAGS; flag++) {
	if ((flag & 31) == 0) {
	    flagmask = user_flags[flag/32];
	}
	if (!(flagnames[flag] && (flagmask & (1<<(flag & 31)))))
	    continue;

	if (strarray_find_case(excluded_flags, flagnames[flag], 0) < 0)
	    strarray_add_case(&event->flagnames, flagnames[flag]);
    }
}

void mboxevent_add_flag(struct mboxevent *event, const char *flag)
{
    if (!event)
	return;

    if (mboxevent_expected_param(event->type, EVENT_FLAG_NAMES))
	strarray_add_case(&event->flagnames, flag);
}

EXPORTED void mboxevent_set_access(struct mboxevent *event,
				   const char *serveraddr, const char *clientaddr,
				   const char *userid, const char *mailboxname)
{
    char url[MAX_MAILBOX_PATH+1];
    struct imapurl imapurl;
    char extname[MAX_MAILBOX_NAME];
    char *userbuf;

    if (!event)
	return;

    /* only notify Logout after successful Login */
    if (!userid && event->type & EVENT_LOGOUT) {
	event->type = EVENT_CANCELLED;
	return;
    }

    /* all events needs uri parameter */
    if (!event->params[EVENT_URI].filled) {
	memset(&imapurl, 0, sizeof(struct imapurl));
	imapurl.server = config_servername;

	if (mailboxname != NULL) {
	    /* translate internal mailbox name to external */
	    assert(namespace.mboxname_toexternal != NULL);
	    (*namespace.mboxname_toexternal)(&namespace, mailboxname,
					     mboxname_to_userid(mailboxname),
					     extname);

	    /* translate any separators in user */
	    userbuf = (char *)mboxname_to_userid(mailboxname);
	    if (userbuf != NULL)
		mboxname_hiersep_toexternal(&namespace, userbuf,
					    config_virtdomains ? strcspn(userbuf, "@") : 0);

	    imapurl.mailbox = extname;
	    imapurl.user = userbuf;
	}

	imapurl_toURL(url, &imapurl);
	FILL_STRING_PARAM(event, EVENT_URI, xstrdup(url));

	if (event->type & MAILBOX_EVENTS) {
	    FILL_STRING_PARAM(event, EVENT_MAILBOX_ID, xstrdup(url));
	}
    }

    if (serveraddr && mboxevent_expected_param(event->type, EVENT_SERVER_ADDRESS)) {
	FILL_STRING_PARAM(event, EVENT_SERVER_ADDRESS, xstrdup(serveraddr));
    }
    if (clientaddr && mboxevent_expected_param(event->type, EVENT_CLIENT_ADDRESS)) {
	FILL_STRING_PARAM(event, EVENT_CLIENT_ADDRESS, xstrdup(clientaddr));
    }
    if (userid && mboxevent_expected_param(event->type, EVENT_USER)) {
	/* translate any separators in user */
	userbuf = xstrdup(userid);
	if (userbuf != NULL)
	    mboxname_hiersep_toexternal(&namespace, userbuf,
				    config_virtdomains ? strcspn(userbuf, "@") : 0);

	FILL_STRING_PARAM(event, EVENT_USER, userbuf);
    }
}

EXPORTED void mboxevent_set_acl(struct mboxevent *event, const char *identifier,
				const char *rights)
{
    if (!event)
	return;

    FILL_STRING_PARAM(event, EVENT_ACL_SUBJECT, xstrdup(identifier));
    FILL_STRING_PARAM(event, EVENT_ACL_RIGHTS, xstrdup(rights));
}

EXPORTED void mboxevent_extract_record(struct mboxevent *event, struct mailbox *mailbox,
				       struct index_record *record)
{
    char *msgid = NULL;

    if (!event)
	return;

    /* add modseq only on first call, cancel otherwise */
    if (mboxevent_expected_param(event->type, EVENT_MODSEQ)) {
	if (event->uidset == NULL || (seqset_first(event->uidset) == seqset_last(event->uidset))) {
	    FILL_UNSIGNED_PARAM(event, EVENT_MODSEQ, record->modseq);
	}
	else {
	    /* From RFC 5423:
	     * modseq May be included with any notification referring
	     * to one message.
	     *
	     * thus cancel inclusion of modseq parameter
	     */
	    event->params[EVENT_MODSEQ].filled = 0;
	}
    }

    /* add UID to uidset */
    if (event->uidset == NULL)
	event->uidset = seqset_init(0, SEQ_SPARSE);
    seqset_add(event->uidset, record->uid, 1);

    if (event->type == EVENT_CANCELLED)
	return;

    /* add Message-Id to midset or NIL if doesn't exists */
    if (mboxevent_expected_param(event->type, (EVENT_MIDSET))) {
	msgid = mailbox_cache_get_msgid(mailbox, record);
	strarray_add(&event->midset, msgid ? msgid : "NIL");

	if (msgid)
	    free(msgid);
    }

    /* add message size */
    if (mboxevent_expected_param(event->type, EVENT_MESSAGE_SIZE)) {
	FILL_UNSIGNED_PARAM(event, EVENT_MESSAGE_SIZE, record->size);
    }

    /* add vnd.cmu.envelope */
    if (mboxevent_expected_param(event->type, EVENT_ENVELOPE)) {
	FILL_STRING_PARAM(event, EVENT_ENVELOPE,
			  xstrndup(cacheitem_base(record, CACHE_ENVELOPE),
				   cacheitem_size(record, CACHE_ENVELOPE)));
    }

    /* add bodyStructure */
    if (mboxevent_expected_param(event->type, EVENT_BODYSTRUCTURE)) {
	FILL_STRING_PARAM(event, EVENT_BODYSTRUCTURE,
			  xstrndup(cacheitem_base(record, CACHE_BODYSTRUCTURE),
				   cacheitem_size(record, CACHE_BODYSTRUCTURE)));
    }
}

void mboxevent_extract_copied_record(struct mboxevent *event,
				     const struct mailbox *mailbox, uint32_t uid)
{
    int first = 0;

    if (!event)
	return;

    /* add the source message's UID to oldUidset */
    if (event->olduidset == NULL) {
	event->olduidset = seqset_init(0, SEQ_SPARSE);
	first = 1;
    }
    seqset_add(event->olduidset, uid, 1);

    /* generate an IMAP URL to reference the old mailbox */
    if (first)
	mboxevent_extract_old_mailbox(event, mailbox);
}

void mboxevent_extract_content(struct mboxevent *event,
			       const struct index_record *record, FILE* content)
{
    const char *base = NULL;
    unsigned long len = 0;
    size_t offset, size, truncate;

    if (!event)
	return;

    if (!mboxevent_expected_param(event->type, EVENT_MESSAGE_CONTENT))
	return;

    truncate = config_getint(IMAPOPT_EVENT_CONTENT_SIZE);

    switch (config_getenum(IMAPOPT_EVENT_CONTENT_INCLUSION_MODE)) {
    /*  include message up to 'truncate' in size with the notification */
    case IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_STANDARD:
	if (!truncate || record->size <= truncate) {
	    offset = 0;
	    size = record->size;
	}
	else {
	    /* XXX RFC 5423 suggests to include a URLAUTH [RFC4467] reference
	     * for larger messages. IMAP URL of mailboxID seems enough though */
	    return;
	}
	break;
    /* include message truncated to a size of 'truncate' */
    case IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_MESSAGE:
	offset = 0;
	size = (truncate && (record->size > truncate)) ?
		truncate : record->size;
	break;
    /* include headers truncated to a size of 'truncate' */
    case IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_HEADER:
	offset = 0;
	size = (truncate && (record->header_size > truncate)) ?
		truncate : record->header_size;
	break;
    /* include body truncated to a size of 'truncate' */
    case IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_BODY:
	offset = record->header_size;
	size = (truncate && ((record->size - record->header_size) > truncate)) ?
		truncate : record->size - record->header_size;
	break;
    /* include full headers and body truncated to a size of 'truncate' */
    case IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_HEADERBODY:
	offset = 0;
	size = (truncate && ((record->size - record->header_size) > truncate)) ?
		record->header_size + truncate : record->size;
	break;
    /* never happen */
    default:
	return;
    }

    map_refresh(fileno(content), 1, &base, &len, record->size, "new message", 0);
    FILL_STRING_PARAM(event, EVENT_MESSAGE_CONTENT, xstrndup(base+offset, size));
    map_free(&base, &len);
}

void mboxevent_extract_quota(struct mboxevent *event, const struct quota *quota,
			     enum quota_resource res)
{
    struct imapurl imapurl;
    char url[MAX_MAILBOX_PATH+1];
    char extname[MAX_MAILBOX_NAME];
    char *userbuf;

    if (!event)
	return;

    switch(res) {
    case QUOTA_STORAGE:
	if (mboxevent_expected_param(event->type, EVENT_DISK_QUOTA)) {
	    if (quota->limits[res] >= 0)
		FILL_UNSIGNED_PARAM(event, EVENT_DISK_QUOTA, quota->limits[res]);
	}
	if (mboxevent_expected_param(event->type, EVENT_DISK_USED)) {
	    FILL_UNSIGNED_PARAM(event, EVENT_DISK_USED,
			   quota->useds[res] / quota_units[res]);
	}
	break;
    case QUOTA_MESSAGE:
	if (quota->limits[res] >= 0)
	    FILL_UNSIGNED_PARAM(event, EVENT_MAX_MESSAGES, quota->limits[res]);
	FILL_UNSIGNED_PARAM(event, EVENT_MESSAGES, quota->useds[res]);
	break;
    default:
	/* others quota are not supported by RFC 5423 */
	break;
    }

    /* From RFC 5423 :
     * The parameters SHOULD include at least the relevant user
     * and quota and, optionally, the mailbox.
     *
     * It seems that it does not correspond to the concept of
     * quota root specified in RFC 2087. Thus we fill uri with quota root
     */
    if (!event->params[EVENT_URI].filled && event->type & QUOTA_EVENTS) {
	/* translate internal mailbox name to external */
	assert(namespace.mboxname_toexternal != NULL);
	(*namespace.mboxname_toexternal)(&namespace, quota->root,
					 mboxname_to_userid(quota->root),
					 extname);

	/* translate any separators in user */
	userbuf = (char *)mboxname_to_userid(quota->root);
	if (userbuf != NULL)
	    mboxname_hiersep_toexternal(&namespace, userbuf,
					config_virtdomains ? strcspn(userbuf, "@") : 0);

	memset(&imapurl, 0, sizeof(struct imapurl));
	imapurl.server = config_servername;
	imapurl.mailbox = extname;
	imapurl.user = userbuf;
	imapurl_toURL(url, &imapurl);
	FILL_STRING_PARAM(event, EVENT_URI, xstrdup(url));
    }
}

EXPORTED void mboxevent_set_numunseen(struct mboxevent *event,
				      struct mailbox *mailbox, int numunseen)
{
    if (!event)
	return;

    if (mboxevent_expected_param(event->type, EVENT_UNSEEN_MESSAGES)) {
	unsigned count = (numunseen >= 0) ? (unsigned)numunseen
					  : mailbox_count_unseen(mailbox);
	/* as event notification is focused on mailbox, we don't care about the
	 * authenticated user but the mailbox's owner.
	 * it could be a problem only if it is a shared or public folder */
	FILL_UNSIGNED_PARAM(event, EVENT_UNSEEN_MESSAGES, count);
    }
}

EXPORTED void mboxevent_extract_mailbox(struct mboxevent *event,
					struct mailbox *mailbox)
{
    struct imapurl imapurl;
    char url[MAX_MAILBOX_PATH+1];
    char extname[MAX_MAILBOX_NAME];
    char *userbuf;

    if (!event)
	return;

    /* mboxevent_extract_mailbox should be called only once */
    if (event->params[EVENT_URI].filled)
	return;

    /* verify if event notification should be disabled for this mailbox  */
    if (!mboxevent_enabled_for_mailbox(mailbox)) {
	event->type = EVENT_CANCELLED;
	return;
    }

    /* translate internal mailbox name to external */
    assert(namespace.mboxname_toexternal != NULL);
    (*namespace.mboxname_toexternal)(&namespace, mailbox->name,
				     mboxname_to_userid(mailbox->name), extname);

    /* translate any separators in user */
    userbuf = (char *)mboxname_to_userid(mailbox->name);
    if (userbuf != NULL)
	mboxname_hiersep_toexternal(&namespace, userbuf,
				    config_virtdomains ? strcspn(userbuf, "@") : 0);

    /* all events needs uri parameter */
    memset(&imapurl, 0, sizeof(struct imapurl));
    imapurl.server = config_servername;
    imapurl.mailbox = extname;
    imapurl.user = userbuf;
    imapurl.uidvalidity = mailbox->i.uidvalidity;
    if (event->type & (EVENT_MESSAGE_NEW|EVENT_MESSAGE_APPEND) && event->uidset) {
	imapurl.uid = seqset_first(event->uidset);
	/* don't add uidset parameter to MessageNew and MessageAppend events */
	seqset_free(event->uidset);
	event->uidset = NULL;
    }

    imapurl_toURL(url, &imapurl);
    FILL_STRING_PARAM(event, EVENT_URI, xstrdup(url));

    /* mailbox related events also require mailboxID */
    if (event->type & MAILBOX_EVENTS) {
	FILL_STRING_PARAM(event, EVENT_MAILBOX_ID, xstrdup(url));
    }
    if (mboxevent_expected_param(event->type, EVENT_UIDNEXT)) {
	FILL_UNSIGNED_PARAM(event, EVENT_UIDNEXT, mailbox->i.last_uid+1);
    }

    /* From RFC 5423 :
     * messages
     *    Included with QuotaExceed and QuotaWithin notifications relating
     *    to a user or mailbox message count quota.  May be included with
     *    other notifications.
     *
     *    Number of messages in the mailbox.  This is typically included
     *    with message addition and deletion events.
     *
     * we are in case messages is relative to the number of messages in the
     * mailbox and not the message count quota.
     */
    if (mboxevent_expected_param(event->type, EVENT_MESSAGES)) {
	FILL_UNSIGNED_PARAM(event, EVENT_MESSAGES, mailbox->i.exists);
    }
}

void mboxevent_extract_old_mailbox(struct mboxevent *event,
				   const struct mailbox *mailbox)
{
    struct imapurl imapurl;
    char url[MAX_MAILBOX_PATH+1];
    char extname[MAX_MAILBOX_NAME];
    char *userbuf;

    if (!event)
	return;

    /* translate internal mailbox name to external */
    assert(namespace.mboxname_toexternal != NULL);
    (*namespace.mboxname_toexternal)(&namespace, mailbox->name,
				     mboxname_to_userid(mailbox->name), extname);

    /* translate any separators in user */
    userbuf = (char *)mboxname_to_userid(mailbox->name);
    if (userbuf != NULL)
	mboxname_hiersep_toexternal(&namespace, userbuf,
				    config_virtdomains ? strcspn(userbuf, "@") : 0);

    memset(&imapurl, 0, sizeof(struct imapurl));
    imapurl.server = config_servername;
    imapurl.mailbox = extname;
    imapurl.user = userbuf;
    imapurl.uidvalidity = mailbox->i.uidvalidity;

    imapurl_toURL(url, &imapurl);
    FILL_STRING_PARAM(event, EVENT_OLD_MAILBOX_ID, xstrdup(url));
}

static const char *event_to_name(enum event_type type)
{
    switch (type) {
    case EVENT_MESSAGE_APPEND:
	return "MessageAppend";
    case EVENT_MESSAGE_EXPIRE:
	return "MessageExpire";
    case EVENT_MESSAGE_EXPUNGE:
	return "MessageExpunge";
    case EVENT_MESSAGE_NEW:
	return "MessageNew";
    case EVENT_MESSAGE_COPY:
	return "vnd.cmu.MessageCopy";
    case EVENT_MESSAGE_MOVE:
	return "vnd.cmu.MessageMove";
    case EVENT_QUOTA_EXCEED:
	return "QuotaExceed";
    case EVENT_QUOTA_WITHIN:
	return "QuotaWithin";
    case EVENT_QUOTA_CHANGE:
	return "QuotaChange";
    case EVENT_MESSAGE_READ:
	return "MessageRead";
    case EVENT_MESSAGE_TRASH:
	return "MessageTrash";
    case EVENT_FLAGS_SET:
	return "FlagsSet";
    case EVENT_FLAGS_CLEAR:
	return "FlagsClear";
    case EVENT_LOGIN:
	return "Login";
    case EVENT_LOGOUT:
	return "Logout";
    case EVENT_MAILBOX_CREATE:
	return "MailboxCreate";
    case EVENT_MAILBOX_DELETE:
	return "MailboxDelete";
    case EVENT_MAILBOX_RENAME:
	return "MailboxRename";
    case EVENT_MAILBOX_SUBSCRIBE:
	return "MailboxSubscribe";
    case EVENT_MAILBOX_UNSUBSCRIBE:
	return "MailboxUnSubscribe";
    case EVENT_ACL_CHANGE:
	return "AclChange";
    default:
	fatal("Unknown message event", EC_SOFTWARE);
    }

    /* never happen */
    return NULL;
}

static char *json_formatter(enum event_type type, struct event_parameter params[])
{
    int param, ival;
    char *val, *ptr, *result;
    json_t *event_json = json_object();
    json_t *jarray;

    json_object_set_new(event_json, "event", json_string(event_to_name(type)));

    for (param = 0; param <= MAX_PARAM; param++) {

	if (!params[param].filled)
	    continue;

	switch (params[param].id) {
	case EVENT_CLIENT_ADDRESS:
	    /* come from saslprops structure */
	    val = strdup((char *)params[param].value);
	    ptr = strchr(val, ';');
	    *ptr++ = '\0';

	    json_object_set_new(event_json, "clientIP", json_string(val));

	    if (parseint32(ptr, (const char **)&ptr, &ival) >= 0)
		json_object_set_new(event_json, "clientPort", json_integer(ival));

	    free(val);
	    break;
	case EVENT_SERVER_ADDRESS:
	    /* come from saslprops structure */
	    val = strdup((char *)params[param].value);
	    ptr = strchr(val, ';');
	    *ptr++ = '\0';

	    json_object_set_new(event_json, "serverDomain", json_string(val));

	    if (parseint32(ptr, (const char **)&ptr, &ival) >= 0)
		json_object_set_new(event_json, "serverPort", json_integer(ival));

	    free(val);
	    break;
	default:
	    switch (params[param].type) {
	    case EVENT_PARAM_INT:
		json_object_set_new(event_json, params[param].name,
				    json_integer(params[param].value));
		break;
	    case EVENT_PARAM_STRING:
		json_object_set_new(event_json, params[param].name,
				    json_string((char *)params[param].value));
		break;
	    case EVENT_PARAM_ARRAY:
		jarray = json_array();
		strarray_t *sarray = (strarray_t *)params[param].value;
		int i;

		for (i = 0; i < strarray_size(sarray); i++) {
		    json_array_append_new(jarray, json_string(strarray_nth(sarray, i)));
		}

		json_object_set_new(event_json, params[param].name, jarray);
		break;
	    }
	    break;
	}
    }

    result = json_dumps(event_json, JSON_PRESERVE_ORDER|JSON_COMPACT);
    json_decref(event_json);

    return result;

}

#ifndef NDEBUG
/* overrides event->type with event_type because FlagsSet may be derived to
 * MessageTrash or MessageRead */
static int filled_params(enum event_type type, struct mboxevent *event)
{
    struct buf missing = BUF_INITIALIZER;
    int param, ret = 1;

    if (!event)
	return 0;

    for (param = 0; param <= MAX_PARAM; param++) {

	if (mboxevent_expected_param(type, param) &&
		!event->params[param].filled) {
	    switch (event->params[param].id) {
	    case EVENT_DISK_QUOTA:
		return event->params[EVENT_MAX_MESSAGES].filled;
	    case EVENT_DISK_USED:
		return event->params[EVENT_MESSAGES].filled;
	    case EVENT_FLAG_NAMES:
		/* flagNames may be included with MessageAppend and MessageNew
		 * also we don't expect it here. */
		if (!(type & (EVENT_MESSAGE_APPEND|EVENT_MESSAGE_NEW)))
		    buf_appendcstr(&missing, " flagNames");
		break;
	    case EVENT_MAX_MESSAGES:
		return event->params[EVENT_DISK_QUOTA].filled;
	    case EVENT_MESSAGE_CONTENT:
		/* messageContent is not included in standard mode if the size
		 * of the message exceed the limit */
		if (config_getenum(IMAPOPT_EVENT_CONTENT_INCLUSION_MODE) !=
		    IMAP_ENUM_EVENT_CONTENT_INCLUSION_MODE_STANDARD)
		    buf_appendcstr(&missing, " messageContent");
		break;
	    case EVENT_MESSAGES:
		return event->params[EVENT_DISK_USED].filled;
	    case EVENT_MODSEQ:
		/* modseq is not included if notification refers to several
		 * messages */
		if (!event->uidset || (seqset_first(event->uidset) == seqset_last(event->uidset)))
		    buf_appendcstr(&missing, " modseq");
		break;
	    default:
		buf_appendcstr(&missing, " ");
		buf_appendcstr(&missing, event->params[param].name);
		break;
	    }
	}
    }

    if (buf_len(&missing)) {
	syslog(LOG_ALERT, "Cannot notify event %s: missing parameters:%s",
	       event_to_name(type), buf_cstring(&missing));
	ret = 0;
    }

    buf_free(&missing);
    return ret;
}
#endif /* NDEBUG */

#else /* !ENABLE_MBOXEVENT */

#include "mboxevent.h"

EXPORTED void mboxevent_init(void)
{
}

EXPORTED void mboxevent_setnamespace(struct namespace *n __attribute__((unused)))
{
}

EXPORTED struct mboxevent *mboxevent_new(enum event_type type __attribute__((unused)))
{
    return NULL;
}

struct mboxevent *mboxevent_enqueue(enum event_type type __attribute__((unused)),
				    struct mboxevent **mboxevents __attribute__((unused)))
{
    return NULL;
}

EXPORTED void mboxevent_free(struct mboxevent **mboxevent __attribute__((unused)))
{
}

void mboxevent_freequeue(struct mboxevent **mboxevent __attribute__((unused)))
{
}

EXPORTED void mboxevent_notify(struct mboxevent *mboxevents __attribute__((unused)))
{
}

void mboxevent_add_flags(struct mboxevent *event __attribute__((unused)),
			 char *flagnames[MAX_USER_FLAGS] __attribute__((unused)),
			 bit32 system_flags __attribute__((unused)),
			 bit32 user_flags[MAX_USER_FLAGS/32] __attribute__((unused)))
{
}

void mboxevent_add_flag(struct mboxevent *event __attribute__((unused)),
			const char *flag __attribute__((unused)))
{
}

EXPORTED void mboxevent_set_access(struct mboxevent *event __attribute__((unused)),
				   const char *serveraddr __attribute__((unused)),
				   const char *clientaddr __attribute__((unused)),
				   const char *userid __attribute__((unused)),
				   const char *mailboxname __attribute__((unused)))
{
}

EXPORTED void mboxevent_set_acl(struct mboxevent *event __attribute__((unused)),
				const char *identifier __attribute__((unused)),
>.......>.......>.......>.......const char *rights __attribute__((unused)))
{
}

EXPORTED void mboxevent_extract_record(struct mboxevent *event __attribute__((unused)),
				       struct mailbox *mailbox __attribute__((unused)),
				       struct index_record *record __attribute__((unused)))
{
}

void mboxevent_extract_copied_record(struct mboxevent *event __attribute__((unused)),
				     const struct mailbox *mailbox __attribute__((unused)),
				     uint32_t uid __attribute__((unused)))
{
}

void mboxevent_extract_content(struct mboxevent *event __attribute__((unused)),
			       const struct index_record *record __attribute__((unused)),
			       FILE* content __attribute__((unused)))
{
}

void mboxevent_extract_quota(struct mboxevent *event __attribute__((unused)),
			     const struct quota *quota __attribute__((unused)),
			     enum quota_resource res __attribute__((unused)))
{
}

EXPORTED void mboxevent_set_numunseen(struct mboxevent *event __attribute__((unused)),
				      struct mailbox *mailbox __attribute__((unused)),
				      int numunseen __attribute__((unused)))
{
}

EXPORTED void mboxevent_extract_mailbox(struct mboxevent *event __attribute__((unused)),
					struct mailbox *mailbox __attribute__((unused)))
{
}

void mboxevent_extract_old_mailbox(struct mboxevent *event __attribute__((unused)),
				   const struct mailbox *mailbox __attribute__((unused)))
{
}

#endif /* !ENABLE_MBOXEVENT */