/* http_jmap.c -- Routines for handling JMAP requests in httpd
 *
 * Copyright (c) 1994-2014 Carnegie Mellon University.  All rights reserved.
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
 */

#include <config.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>
#include <jansson.h>

#include "acl.h"
#include "annotate.h"
#include "append.h"
#include "carddav_db.h"
#include "global.h"
#include "hash.h"
#include "httpd.h"
#include "http_proxy.h"
#include "mailbox.h"
#include "mboxlist.h"
#include "statuscache.h"
#include "times.h"
#include "util.h"
#include "version.h"
#include "xmalloc.h"
#include "xstrlcat.h"
#include "xstrlcpy.h"

/* generated headers are not necessarily in current directory */
#include "imap/http_err.h"

struct jmap_req {
    const char *userid;
    struct auth_state *authstate;
    struct hash_table *idmap;
    json_t *args;
    json_t *response;
    const char *state; // if changing things, this is pre-change state
    const char *tag;
};

struct namespace jmap_namespace;

static time_t compile_time;
static void jmap_init(struct buf *serverinfo);
static void jmap_auth(const char *userid);
static int meth_get(struct transaction_t *txn, void *params);
static int meth_post(struct transaction_t *txn, void *params);
static int getMailboxes(struct jmap_req *req);
static int getContactGroups(struct jmap_req *req);
static int getContactGroupUpdates(struct jmap_req *req);
static int setContactGroups(struct jmap_req *req);
static int getContacts(struct jmap_req *req);
static int getContactUpdates(struct jmap_req *req);
static int setContacts(struct jmap_req *req);

static const struct message_t {
    const char *name;
    int (*proc)(struct jmap_req *req);
} messages[] = {
    { "getMailboxes",   &getMailboxes },
    { "getContactGroups",       &getContactGroups },
    { "getContactGroupUpdates", &getContactGroupUpdates },
    { "setContactGroups",       &setContactGroups },
    { "getContacts",            &getContacts },
    { "getContactUpdates",      &getContactUpdates },
    { "setContacts",            &setContacts },
    { NULL,             NULL}
};


/* Namespace for JMAP */
struct namespace_t namespace_jmap = {
    URL_NS_JMAP, 0, "/jmap", NULL, 1 /* auth */, (ALLOW_READ | ALLOW_POST),
    /*type*/0, &jmap_init, &jmap_auth, NULL, NULL,
    {
        { NULL,                 NULL },                 /* ACL          */
        { NULL,                 NULL },                 /* COPY         */
        { NULL,                 NULL },                 /* DELETE       */
        { &meth_get,            NULL },                 /* GET          */
        { &meth_get,            NULL },                 /* HEAD         */
        { NULL,                 NULL },                 /* LOCK         */
        { NULL,                 NULL },                 /* MKCALENDAR   */
        { NULL,                 NULL },                 /* MKCOL        */
        { NULL,                 NULL },                 /* MOVE         */
        { &meth_options,        NULL },                 /* OPTIONS      */
        { &meth_post,           NULL },                 /* POST */
        { NULL,                 NULL },                 /* PROPFIND     */
        { NULL,                 NULL },                 /* PROPPATCH    */
        { NULL,                 NULL },                 /* PUT          */
        { NULL,                 NULL },                 /* REPORT       */
        { &meth_trace,          NULL },                 /* TRACE        */
        { NULL,                 NULL }                  /* UNLOCK       */
    }
};


static void jmap_init(struct buf *serverinfo __attribute__((unused)))
{
    namespace_jmap.enabled =
        config_httpmodules & IMAP_ENUM_HTTPMODULES_JMAP;

    if (!namespace_jmap.enabled) return;

    compile_time = calc_compile_time(__TIME__, __DATE__);
}


static void jmap_auth(const char *userid __attribute__((unused)))
{
    /* Set namespace */
    mboxname_init_namespace(&jmap_namespace,
                            httpd_userisadmin || httpd_userisproxyadmin);
}


/* Perform a GET/HEAD request */
static int meth_get(struct transaction_t *txn __attribute__((unused)),
                     void *params __attribute__((unused)))
{
    return HTTP_NO_CONTENT;
}

/* Perform a POST request */
static int meth_post(struct transaction_t *txn,
                     void *params __attribute__((unused)))
{
    const char **hdr;
    json_t *req, *resp = NULL;
    json_error_t jerr;
    const struct message_t *mp = NULL;
    struct mailbox *mailbox = NULL;
    struct hash_table idmap;
    size_t i, flags = JSON_PRESERVE_ORDER;
    int ret;
    char *buf;

    construct_hash_table(&idmap, 1024, 0);

    /* Read body */
    txn->req_body.flags |= BODY_DECODE;
    ret = http_read_body(httpd_in, httpd_out,
                       txn->req_hdrs, &txn->req_body, &txn->error.desc);
    if (ret) {
        txn->flags.conn = CONN_CLOSE;
        return ret;
    }

    if (!buf_len(&txn->req_body.payload)) return HTTP_BAD_REQUEST;

    /* Check Content-Type */
    if (!(hdr = spool_getheader(txn->req_hdrs, "Content-Type")) ||
        !is_mediatype("application/json", hdr[0])) {
        txn->error.desc = "This method requires a JSON request body\r\n";
        return HTTP_BAD_MEDIATYPE;
    }

    /* Parse the JSON request */
    req = json_loads(buf_cstring(&txn->req_body.payload), 0, &jerr);
    if (!req || !json_is_array(req)) {
        txn->error.desc = "Unable to parse JSON request body\r\n";
        ret = HTTP_BAD_REQUEST;
        goto done;
    }

    /* Start JSON response */
    resp = json_array();
    if (!resp) {
        txn->error.desc = "Unable to create JSON response body\r\n";
        ret = HTTP_SERVER_ERROR;
        goto done;
    }

    const char *inboxname = mboxname_user_mbox(httpd_userid, NULL);

    /* we lock the user's INBOX before we start any operation, because that way we
     * guarantee (via conversations magic) that nothing changes the modseqs except
     * our operations */
    int r = mailbox_open_iwl(inboxname, &mailbox);
    if (r) {
        txn->error.desc = error_message(r);
        ret = HTTP_SERVER_ERROR;
        goto done;
    }

    /* Process each message in the request */
    for (i = 0; i < json_array_size(req); i++) {
        json_t *msg = json_array_get(req, i);
        const char *name = json_string_value(json_array_get(msg, 0));
        json_t *args = json_array_get(msg, 1);
        json_t *id = json_array_get(msg, 2);
        /* XXX - better error reporting */
        if (!id) continue;
        const char *tag = json_string_value(id);
        int r = 0;

        /* Find the message processor */
        for (mp = messages; mp->name && strcmp(name, mp->name); mp++);

        if (!mp || !mp->name) {
            json_array_append(resp, json_pack("[s {s:s} s]", "error", "type", "unknownMethod", tag));
            continue;
        }

        /* read the modseq again every time, just in case something changed it
         * in our actions */
        struct buf buf = BUF_INITIALIZER;
        modseq_t modseq = mboxname_readmodseq(inboxname);
        buf_printf(&buf, "%llu", modseq);

        struct jmap_req req;
        req.userid = httpd_userid;
        req.authstate = httpd_authstate;
        req.args = args;
        req.state = buf_cstring(&buf);
        req.response = resp;
        req.tag = tag;
        req.idmap = &idmap;

        r = mp->proc(&req);

        buf_free(&buf);

        if (r) {
            txn->error.desc = error_message(r);
            ret = HTTP_SERVER_ERROR;
            goto done;
        }
    }

    /* unlock here so that we don't block on writing */
    mailbox_unlock_index(mailbox, NULL);

    /* Dump JSON object into a text buffer */
    flags |= (config_httpprettytelemetry ? JSON_INDENT(2) : JSON_COMPACT);
    buf = json_dumps(resp, flags);

    if (!buf) {
        txn->error.desc = "Error dumping JSON response object";
        ret = HTTP_SERVER_ERROR;
        goto done;
    }

    /* Output the JSON object */
    txn->resp_body.type = "application/json; charset=utf-8";
    write_body(HTTP_OK, txn, buf, strlen(buf));
    free(buf);

  done:
    free_hash_table(&idmap, free);
    mailbox_close(&mailbox);
    if (req) json_decref(req);
    if (resp) json_decref(resp);

    return ret;
}


/* mboxlist_findall() callback to list mailboxes */
int getMailboxes_cb(const char *mboxname, int matchlen __attribute__((unused)),
                    int maycreate __attribute__((unused)),
                    void *rock)
{
    json_t *list = (json_t *) rock, *mbox;
    struct mboxlist_entry *mbentry = NULL;
    struct mailbox *mailbox = NULL;
    int r = 0, rights;
    unsigned statusitems = STATUS_MESSAGES | STATUS_UNSEEN;
    struct statusdata sdata;

    /* Check ACL on mailbox for current user */
    if ((r = mboxlist_lookup(mboxname, &mbentry, NULL))) {
        syslog(LOG_INFO, "mboxlist_lookup(%s) failed: %s",
               mboxname, error_message(r));
        goto done;
    }

    rights = mbentry->acl ? cyrus_acl_myrights(httpd_authstate, mbentry->acl) : 0;
    if ((rights & (ACL_LOOKUP | ACL_READ)) != (ACL_LOOKUP | ACL_READ)) {
        goto done;
    }

    /* Open mailbox to get uniqueid */
    if ((r = mailbox_open_irl(mboxname, &mailbox))) {
        syslog(LOG_INFO, "mailbox_open_irl(%s) failed: %s",
               mboxname, error_message(r));
        goto done;
    }
    mailbox_unlock_index(mailbox, NULL);

    r = status_lookup(mboxname, httpd_userid, statusitems, &sdata);

    mbox = json_pack("{s:s s:s s:n s:n s:b s:b s:b s:b s:i s:i}",
                     "id", mailbox->uniqueid,
                     "name", mboxname,
                     "parentId",
                     "role",
                     "mayAddMessages", rights & ACL_INSERT,
                     "mayRemoveMessages", rights & ACL_DELETEMSG,
                     "mayCreateChild", rights & ACL_CREATE,
                     "mayDeleteMailbox", rights & ACL_DELETEMBOX,
                     "totalMessages", sdata.messages,
                     "unreadMessages", sdata.unseen);
    json_array_append_new(list, mbox);

    mailbox_close(&mailbox);

  done:

    return 0;
}


/* Execute a getMailboxes message */
static int getMailboxes(struct jmap_req *req)
{
    json_t *item, *mailboxes, *list;

    /* Start constructing our response */
    item = json_pack("[s {s:s s:s} s]", "mailboxes",
                     "accountId", req->userid,
                     "state", req->state,
                     req->tag);

    list = json_array();

    /* Generate list of mailboxes */
    int isadmin = httpd_userisadmin||httpd_userisproxyadmin;
    mboxlist_findall(&jmap_namespace, "*", isadmin, httpd_userid,
                     httpd_authstate, &getMailboxes_cb, list);

    mailboxes = json_array_get(item, 1);
    json_object_set_new(mailboxes, "list", list);

    /* xxx - args */
    json_object_set_new(mailboxes, "notFound", json_null());

    json_array_append_new(req->response, item);

    return 0;
}

static void _add_xhref(json_t *obj, const char *mboxname, const char *resource)
{
    /* XXX - look up root path from namespace? */
    struct buf buf = BUF_INITIALIZER;
    const char *userid = mboxname_to_userid(mboxname);
    if (strchr(userid, '@')) {
        buf_printf(&buf, "%s/user/%s/%s/%s",
                   namespace_addressbook.prefix,
                   userid, strrchr(mboxname, '.')+1,
                   resource);
    }
    else {
        const char *domain =
            httpd_extradomain ? httpd_extradomain : config_defdomain;
        buf_printf(&buf, "%s/user/%s@%s/%s/%s",
                   namespace_addressbook.prefix,
                   userid, domain, strrchr(mboxname, '.')+1,
                   resource);
    }
    json_object_set_new(obj, "x-href", json_string(buf_cstring(&buf)));
    buf_free(&buf);
}

struct cards_rock {
    struct jmap_req *req;
    json_t *array;
    struct hash_table *need;
    struct hash_table *props;
    struct mailbox *mailbox;
    int mboxoffset;
};

static int getgroups_cb(void *rock, struct carddav_data *cdata)
{
    struct cards_rock *crock = (struct cards_rock *) rock;
    struct index_record record;
    int r;

    if (crock->need) {
        /* skip records not in hash */
        if (!hash_lookup(cdata->vcard_uid, crock->need))
            return 0;
        /* mark 2 == seen */
        hash_insert(cdata->vcard_uid, (void *)2, crock->need);
    }

    if (!crock->mailbox || strcmp(crock->mailbox->name, cdata->dav.mailbox)) {
        mailbox_close(&crock->mailbox);
        r = mailbox_open_irl(cdata->dav.mailbox, &crock->mailbox);
        if (r) return r;
    }

    r = mailbox_find_index_record(crock->mailbox, cdata->dav.imap_uid, &record);
    if (r) return r;

    /* XXX - this could definitely be refactored from here and mailbox.c */
    struct buf msg_buf = BUF_INITIALIZER;
    struct vparse_state vparser;
    struct vparse_entry *ventry = NULL;

    /* Load message containing the resource and parse vcard data */
    r = mailbox_map_record(crock->mailbox, &record, &msg_buf);
    if (r) return r;

    memset(&vparser, 0, sizeof(struct vparse_state));
    vparser.base = buf_cstring(&msg_buf) + record.header_size;
    r = vparse_parse(&vparser, 0);
    buf_free(&msg_buf);
    if (r) return r;
    if (!vparser.card || !vparser.card->objects) {
        vparse_free(&vparser);
        return r;
    }
    struct vparse_card *vcard = vparser.card->objects;

    json_t *obj = json_pack("{}");

    json_object_set_new(obj, "id", json_string(cdata->vcard_uid));

    json_object_set_new(obj, "addressbookId",
                        json_string(cdata->dav.mailbox + crock->mboxoffset));

    json_t *contactids = json_pack("[]");
    json_t *otherids = json_pack("{}");

    _add_xhref(obj, cdata->dav.mailbox, cdata->dav.resource);

    for (ventry = vcard->properties; ventry; ventry = ventry->next) {
        const char *name = ventry->name;
        const char *propval = ventry->v.value;

        if (!name) continue;
        if (!propval) continue;

        if (!strcmp(name, "fn")) {
            json_object_set_new(obj, "name", json_string(propval));
        }

        else if (!strcmp(name, "x-addressbookserver-member")) {
            if (strncmp(propval, "urn:uuid:", 9)) continue;
            json_array_append_new(contactids, json_string(propval+9));
        }

        else if (!strcmp(name, "x-fm-otheraccount-member")) {
            if (strncmp(propval, "urn:uuid:", 9)) continue;
            struct vparse_param *param = vparse_get_param(ventry, "userid");
            json_t *object = json_object_get(otherids, param->value);
            if (!object) {
                object = json_array();
                json_object_set_new(otherids, param->value, object);
            }
            json_array_append_new(object, json_string(propval+9));
        }
    }
    json_object_set_new(obj, "contactIds", contactids);
    json_object_set_new(obj, "otherAccountContactIds", otherids);

    json_array_append_new(crock->array, obj);

    return 0;
}

static void _add_notfound(const char *key, void *data, void *rock)
{
    json_t *list = (json_t *)rock;
    /* magic "pointer" of 1 equals wanted but not found */
    if (data == (void *)1)
        json_array_append_new(list, json_string(key));
}

static int getContactGroups(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    const char *addressbookId = "Default";
    json_t *abookid = json_object_get(req->args, "addressbookId");
    if (abookid && json_string_value(abookid)) {
        /* XXX - invalid arguments */
        addressbookId = json_string_value(abookid);
    }
    const char *abookname = mboxname_abook(req->userid, addressbookId);

    struct cards_rock rock;
    int r;

    rock.array = json_pack("[]");
    rock.need = NULL;
    rock.props = NULL;
    rock.mailbox = NULL;
    rock.mboxoffset = strlen(abookname) - strlen(addressbookId);

    json_t *want = json_object_get(req->args, "ids");
    if (want) {
        rock.need = xzmalloc(sizeof(struct hash_table));
        construct_hash_table(rock.need, 1024, 0);
        int i;
        int size = json_array_size(want);
        for (i = 0; i < size; i++) {
            const char *id = json_string_value(json_array_get(want, i));
            if (id == NULL) {
                free_hash_table(rock.need, NULL);
                free(rock.need);
                return -1; /* XXX - need codes */
            }
            /* 1 == want */
            hash_insert(id, (void *)1, rock.need);
        }
    }

    r = carddav_get_cards(db, abookname, CARDDAV_KIND_GROUP,
                          &getgroups_cb, &rock);
    if (r) goto done;

    json_t *contactGroups = json_pack("{}");
    json_object_set_new(contactGroups, "accountId", json_string(req->userid));
    json_object_set_new(contactGroups, "state", json_string(req->state));
    json_object_set_new(contactGroups, "list", rock.array);
    if (rock.need) {
        json_t *notfound = json_array();
        hash_enumerate(rock.need, _add_notfound, notfound);
        free_hash_table(rock.need, NULL);
        free(rock.need);
        if (json_array_size(notfound)) {
            json_object_set_new(contactGroups, "notFound", notfound);
        }
        else {
            json_decref(notfound);
            json_object_set_new(contactGroups, "notFound", json_null());
        }
    }
    else {
        json_object_set_new(contactGroups, "notFound", json_null());
    }

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contactGroups"));
    json_array_append_new(item, contactGroups);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

  done:
    mailbox_close(&rock.mailbox);
    carddav_close(db);
    return r;
}


static const char *_json_object_get_string(const json_t *obj, const char *key)
{
    const json_t *jval = json_object_get(obj, key);
    if (!jval) return NULL;
    const char *val = json_string_value(jval);
    return val;
}

static const char *_json_array_get_string(const json_t *obj, size_t index)
{
    const json_t *jval = json_array_get(obj, index);
    if (!jval) return NULL;
    const char *val = json_string_value(jval);
    return val;
}

struct updates_rock {
    json_t *changed;
    json_t *removed;
};

static void strip_spurious_deletes(struct updates_rock *urock)
{
    /* if something is mentioned in both DELETEs and UPDATEs, it's probably
     * a move.  O(N*M) algorithm, but there are rarely many, and the alternative
     * of a hash will cost more */
    unsigned i, j;

    for (i = 0; i < json_array_size(urock->removed); i++) {
        const char *del = json_string_value(json_array_get(urock->removed, i));

        for (j = 0; j < json_array_size(urock->changed); j++) {
            const char *up =
                json_string_value(json_array_get(urock->changed, j));
            if (!strcmpsafe(del, up)) {
                json_array_remove(urock->removed, i--);
                break;
            }
        }
    }
}

static int getupdates_cb(void *rock, struct carddav_data *cdata)
{
    struct updates_rock *urock = (struct updates_rock *) rock;

    if (cdata->dav.alive) {
        json_array_append_new(urock->changed, json_string(cdata->vcard_uid));
    }
    else {
        json_array_append_new(urock->removed, json_string(cdata->vcard_uid));
    }

    return 0;
}

static int getContactGroupUpdates(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    const char *since = _json_object_get_string(req->args, "sinceState");
    if (!since) return -1;
    modseq_t oldmodseq = str2uint64(since);

    struct updates_rock rock;
    rock.changed = json_array();
    rock.removed = json_array();

    int r = carddav_get_updates(db, oldmodseq, CARDDAV_KIND_GROUP,
                                &getupdates_cb, &rock);
    if (r) goto done;

    strip_spurious_deletes(&rock);

    json_t *contactGroupUpdates = json_pack("{}");
    json_object_set_new(contactGroupUpdates, "accountId",
                        json_string(req->userid));
    json_object_set_new(contactGroupUpdates, "oldState",
                        json_string(since)); // XXX - just use refcounted
    json_object_set_new(contactGroupUpdates, "newState",
                        json_string(req->state));
    json_object_set(contactGroupUpdates, "changed", rock.changed);
    json_object_set(contactGroupUpdates, "removed", rock.removed);

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contactGroupUpdates"));
    json_array_append_new(item, contactGroupUpdates);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

    json_t *dofetch = json_object_get(req->args, "fetchContactGroups");
    if (dofetch && json_is_true(dofetch) && json_array_size(rock.changed)) {
        struct jmap_req subreq = *req; // struct copy, woot
        subreq.args = json_pack("{}");
        json_object_set(subreq.args, "ids", rock.changed);
        json_t *abookid = json_object_get(req->args, "addressbookId");
        if (abookid) {
            json_object_set(subreq.args, "addressbookId", abookid);
        }
        r = getContactGroups(&subreq);
        json_decref(subreq.args);
    }

    json_decref(rock.changed);
    json_decref(rock.removed);

  done:
    carddav_close(db);
    return r;
}

static void _card_val(struct vparse_card *card, const char *name,
                      const char *value)
{
    struct vparse_entry *res = vparse_get_entry(card, NULL, name);
    if (!res) res = vparse_add_entry(card, NULL, name, NULL);
    free(res->v.value);
    res->v.value = xstrdupnull(value);
}

static int carddav_store(struct mailbox *mailbox, struct vparse_card *vcard,
                         const char *resource,
                         strarray_t *flags, struct entryattlist *annots,
                         const char *userid, struct auth_state *authstate)
{
    int r = 0;
    FILE *f = NULL;
    struct stagemsg *stage;
    char *header;
    quota_t qdiffs[QUOTA_NUMRESOURCES] = QUOTA_DIFFS_DONTCARE_INITIALIZER;
    struct appendstate as;
    time_t now = time(0);
    char *freeme = NULL;
    char datestr[80];

    /* Prepare to stage the message */
    if (!(f = append_newstage(mailbox->name, now, 0, &stage))) {
        syslog(LOG_ERR, "append_newstage(%s) failed", mailbox->name);
        return -1;
    }

    /* set the REVision time */
    time_to_iso8601(now, datestr, sizeof(datestr), 0);
    _card_val(vcard, "REV", datestr);

    /* Create header for resource */
    const char *uid = vparse_stringval(vcard, "uid");
    const char *fullname = vparse_stringval(vcard, "fn");
    if (!resource) resource = freeme = strconcat(uid, ".vcf", (char *)NULL);
    struct buf buf = BUF_INITIALIZER;
    vparse_tobuf(vcard, &buf);
    const char *mbuserid = mboxname_to_userid(mailbox->name);

    time_to_rfc822(now, datestr, sizeof(datestr));

    /* XXX  This needs to be done via an LDAP/DB lookup */
    header = charset_encode_mimeheader(mbuserid, 0);
    fprintf(f, "From: %s <>\r\n", header);
    free(header);

    header = charset_encode_mimeheader(fullname, 0);
    fprintf(f, "Subject: %s\r\n", header);
    free(header);

    fprintf(f, "Date: %s\r\n", datestr);

    if (strchr(uid, '@'))
        fprintf(f, "Message-ID: <%s>\r\n", uid);
    else
        fprintf(f, "Message-ID: <%s@%s>\r\n", uid, config_servername);

    fprintf(f, "Content-Type: text/vcard; charset=utf-8\r\n");

    fprintf(f, "Content-Length: %u\r\n", (unsigned)buf_len(&buf));
    fprintf(f, "Content-Disposition: inline; filename=\"%s\"\r\n", resource);

    /* XXX  Check domain of data and use appropriate CTE */

    fprintf(f, "MIME-Version: 1.0\r\n");
    fprintf(f, "\r\n");

    /* Write the vCard data to the file */
    fprintf(f, "%s", buf_cstring(&buf));
    buf_free(&buf);

    qdiffs[QUOTA_STORAGE] = ftell(f);
    qdiffs[QUOTA_MESSAGE] = 1;

    fclose(f);

    if ((r = append_setup_mbox(&as, mailbox, userid, authstate, 0,
                               ignorequota ? NULL : qdiffs, 0, 0,
                               EVENT_MESSAGE_NEW|EVENT_CALENDAR))) {
        syslog(LOG_ERR, "append_setup(%s) failed: %s",
               mailbox->name, error_message(r));
        goto done;
    }

    struct body *body = NULL;

    r = append_fromstage(&as, &body, stage, now, flags, 0, annots);
    if (body) {
        message_free_body(body);
        free(body);
    }

    if (r) {
        syslog(LOG_ERR, "append_fromstage() failed");
        append_abort(&as);
        goto done;
    }

    /* Commit the append to the calendar mailbox */
    r = append_commit(&as);
    if (r) {
        syslog(LOG_ERR, "append_commit() failed");
        goto done;
    }

done:
    append_removestage(stage);
    free(freeme);
    return r;
}

static int carddav_remove(struct mailbox *mailbox,
                          uint32_t olduid, int isreplace)
{

    int userflag;
    int r = mailbox_user_flag(mailbox, DFLAG_UNBIND, &userflag, 1);
    struct index_record oldrecord;
    if (!r) r = mailbox_find_index_record(mailbox, olduid, &oldrecord);
    if (!r && !(oldrecord.system_flags & FLAG_EXPUNGED)) {
        if (isreplace) oldrecord.user_flags[userflag/32] |= 1<<(userflag&31);
        oldrecord.system_flags |= FLAG_EXPUNGED;
        r = mailbox_rewrite_index_record(mailbox, &oldrecord);
    }
    if (r) {
        syslog(LOG_ERR, "expunging record (%s) failed: %s",
               mailbox->name, error_message(r));
    }
    return r;
}

static const char *_resolveid(struct jmap_req *req, const char *id)
{
    const char *newid = hash_lookup(id, req->idmap);
    if (newid) return newid;
    return id;
}

static int _add_group_entries(struct jmap_req *req,
                              struct vparse_card *card, json_t *members)
{
    vparse_delete_entries(card, NULL, "X-ADDRESSBOOKSERVER-MEMBER");
    int r = 0;
    size_t index;
    struct buf buf = BUF_INITIALIZER;

    for (index = 0; index < json_array_size(members); index++) {
        const char *item = _json_array_get_string(members, index);
        if (!item) continue;
        const char *uid = _resolveid(req, item);
        buf_setcstr(&buf, "urn:uuid:");
        buf_appendcstr(&buf, uid);
        vparse_add_entry(card, NULL,
                         "X-ADDRESSBOOKSERVER-MEMBER", buf_cstring(&buf));
    }

    buf_free(&buf);
    return r;
}

static int _add_othergroup_entries(struct jmap_req *req,
                                   struct vparse_card *card, json_t *members)
{
    vparse_delete_entries(card, NULL, "X-FM-OTHERACCOUNT-MEMBER");
    int r = 0;
    struct buf buf = BUF_INITIALIZER;
    const char *key;
    json_t *arg;
    json_object_foreach(members, key, arg) {
        unsigned i;
        for (i = 0; i < json_array_size(arg); i++) {
            const char *item = json_string_value(json_array_get(arg, i));
            if (!item)
                return -1;
            const char *uid = _resolveid(req, item);
            buf_setcstr(&buf, "urn:uuid:");
            buf_appendcstr(&buf, uid);
            struct vparse_entry *entry =
                vparse_add_entry(card, NULL,
                                 "X-FM-OTHERACCOUNT-MEMBER", buf_cstring(&buf));
            vparse_add_param(entry, "userid", key);
        }
    }
    buf_free(&buf);
    return r;
}

static int setContactGroups(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    int r = 0;
    json_t *jcheckState = json_object_get(req->args, "ifInState");
    if (jcheckState) {
        const char *checkState = json_string_value(jcheckState);
        if (!checkState ||strcmp(req->state, checkState)) {
            json_t *item = json_pack("[s, {s:s}, s]",
                                     "error", "type", "stateMismatch", req->tag);
            json_array_append_new(req->response, item);
            return 0;
        }
    }
    json_t *set = json_pack("{s:s,s:s}",
                            "oldState", req->state,
                            "accountId", req->userid);

    struct mailbox *mailbox = NULL;
    struct mailbox *newmailbox = NULL;

    json_t *create = json_object_get(req->args, "create");
    if (create) {
        json_t *created = json_pack("{}");
        json_t *notCreated = json_pack("{}");
        json_t *record;

        const char *key;
        json_t *arg;
        json_object_foreach(create, key, arg) {
            const char *uid = makeuuid();
            json_t *jname = json_object_get(arg, "name");
            if (!jname) {
                json_t *err = json_pack("{s:s}", "type", "missingParameters");
                json_object_set_new(notCreated, key, err);
                continue;
            }
            const char *name = json_string_value(jname);
            if (!name) {
                json_t *err = json_pack("{s:s}", "type", "invalidArguments");
                json_object_set_new(notCreated, key, err);
                continue;
            }
            // XXX - no name => notCreated
            struct vparse_card *card = vparse_new_card("VCARD");
            vparse_add_entry(card, NULL, "VERSION", "3.0");
            vparse_add_entry(card, NULL, "FN", name);
            vparse_add_entry(card, NULL, "UID", uid);
            vparse_add_entry(card, NULL, "X-ADDRESSBOOKSERVER-KIND", "group");

            /* it's legal to create an empty group */
            json_t *members = json_object_get(arg, "contactIds");
            if (members) {
                r = _add_group_entries(req, card, members);
                if (r) {
                    /* this one is legit -
                       it just means we'll be adding an error instead */
                    r = 0;
                    json_t *err = json_pack("{s:s}",
                                            "type", "invalidContactId");
                    json_object_set_new(notCreated, key, err);
                    vparse_free_card(card);
                    continue;
                }
            }

            /* it's legal to create an empty group */
            json_t *others = json_object_get(arg, "otherAccountContactIds");
            if (others) {
                r = _add_othergroup_entries(req, card, others);
                if (r) {
                    /* this one is legit -
                       it just means we'll be adding an error instead */
                    r = 0;
                    json_t *err = json_pack("{s:s}",
                                            "type", "invalidContactId");
                    json_object_set_new(notCreated, key, err);
                    vparse_free_card(card);
                    continue;
                }
            }

            const char *addressbookId = "Default";
            json_t *abookid = json_object_get(arg, "addressbookId");
            if (abookid && json_string_value(abookid)) {
                /* XXX - invalid arguments */
                addressbookId = json_string_value(abookid);
            }
            const char *mboxname = mboxname_abook(req->userid, addressbookId);
            json_object_del(arg, "addressbookId");
            addressbookId = NULL;

            /* we need to create and append a record */
            if (!mailbox || strcmp(mailbox->name, mboxname)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(mboxname, &mailbox);
            }

            syslog(LOG_NOTICE, "jmap: create group %s/%s/%s (%s)",
                   req->userid, addressbookId, uid, name);

            if (!r) r = carddav_store(mailbox, card, NULL, NULL, NULL,
                                      req->userid, req->authstate);

            vparse_free_card(card);

            if (r) {
                /* these are real "should never happen" errors */
                goto done;
            }

            record = json_pack("{s:s}", "id", uid);
            json_object_set_new(created, key, record);

            /* hash_insert takes ownership of uid here, skanky I know */
            hash_insert(key, xstrdup(uid), req->idmap);
        }

        if (json_object_size(created))
            json_object_set(set, "created", created);
        json_decref(created);
        if (json_object_size(notCreated))
            json_object_set(set, "notCreated", notCreated);
        json_decref(notCreated);
    }

    json_t *update = json_object_get(req->args, "update");
    if (update) {
        json_t *updated = json_pack("[]");
        json_t *notUpdated = json_pack("{}");

        const char *uid;
        json_t *arg;
        json_object_foreach(update, uid, arg) {
            struct carddav_data *cdata = NULL;
            r = carddav_lookup_uid(db, uid, &cdata);
            uint32_t olduid;
            char *resource = NULL;

            /* is it a valid group? */
            if (r || !cdata || !cdata->dav.imap_uid || !cdata->dav.resource
                  || cdata->kind != CARDDAV_KIND_GROUP) {
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "notFound");
                json_object_set_new(notUpdated, uid, err);
                continue;
            }
            olduid = cdata->dav.imap_uid;
            resource = xstrdup(cdata->dav.resource);

            if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(cdata->dav.mailbox, &mailbox);
                if (r) {
                    syslog(LOG_ERR, "IOERROR: failed to open %s",
                           cdata->dav.mailbox);
                    goto done;
                }
            }

            json_t *abookid = json_object_get(arg, "addressbookId");
            if (abookid && json_string_value(abookid)) {
                const char *mboxname =
                    mboxname_abook(req->userid, json_string_value(abookid));
                if (strcmp(mboxname, cdata->dav.mailbox)) {
                    /* move */
                    r = mailbox_open_iwl(mboxname, &newmailbox);
                    if (r) {
                        syslog(LOG_ERR, "IOERROR: failed to open %s", mboxname);
                        goto done;
                    }
                }
                json_object_del(arg, "addressbookId");
            }

            /* XXX - this could definitely be refactored from here and mailbox.c */
            struct buf msg_buf = BUF_INITIALIZER;
            struct vparse_state vparser;
            struct index_record record;

            r = mailbox_find_index_record(mailbox,
                                          cdata->dav.imap_uid, &record);
            if (r) goto done;

            /* Load message containing the resource and parse vcard data */
            r = mailbox_map_record(mailbox, &record, &msg_buf);
            if (r) goto done;

            memset(&vparser, 0, sizeof(struct vparse_state));
            vparser.base = buf_cstring(&msg_buf) + record.header_size;
            vparse_set_multival(&vparser, "adr");
            vparse_set_multival(&vparser, "org");
            vparse_set_multival(&vparser, "n");
            r = vparse_parse(&vparser, 0);
            buf_free(&msg_buf);
            if (r || !vparser.card || !vparser.card->objects) {
                json_t *err = json_pack("{s:s}", "type", "parseError");
                json_object_set_new(notUpdated, uid, err);
                vparse_free(&vparser);
                mailbox_close(&newmailbox);
                continue;
            }
            struct vparse_card *card = vparser.card->objects;

            json_t *namep = json_object_get(arg, "name");
            if (namep) {
                const char *name = json_string_value(namep);
                if (!name) {
                    json_t *err = json_pack("{s:s}",
                                            "type", "invalidArguments");
                    json_object_set_new(notUpdated, uid, err);
                    vparse_free(&vparser);
                    mailbox_close(&newmailbox);
                    continue;
                }
                struct vparse_entry *entry = vparse_get_entry(card, NULL, "FN");
                if (entry) {
                    free(entry->v.value);
                    entry->v.value = xstrdup(name);
                }
                else {
                    vparse_add_entry(card, NULL, "FN", name);
                }
            }

            json_t *members = json_object_get(arg, "contactIds");
            if (members) {
                r = _add_group_entries(req, card, members);
                if (r) {
                    /* this one is legit -
                       it just means we'll be adding an error instead */
                    r = 0;
                    json_t *err = json_pack("{s:s}",
                                            "type", "invalidContactId");
                    json_object_set_new(notUpdated, uid, err);
                    vparse_free(&vparser);
                    mailbox_close(&newmailbox);
                    continue;
                }
            }

            json_t *others = json_object_get(arg, "otherAccountContactIds");
            if (others) {
                r = _add_othergroup_entries(req, card, others);
                if (r) {
                    /* this one is legit -
                       it just means we'll be adding an error instead */
                    r = 0;
                    json_t *err = json_pack("{s:s}",
                                            "type", "invalidContactId");
                    json_object_set_new(notUpdated, uid, err);
                    vparse_free(&vparser);
                    mailbox_close(&newmailbox);
                    continue;
                }
            }

            syslog(LOG_NOTICE, "jmap: update group %s/%s",
                   req->userid, resource);

            r = carddav_store(newmailbox ? newmailbox : mailbox, card, resource,
                              NULL, NULL, req->userid, req->authstate);
            if (!r)
                r = carddav_remove(mailbox, olduid, /*isreplace*/!newmailbox);
            mailbox_close(&newmailbox);

            vparse_free(&vparser);
            free(resource);
            if (r) goto done;

            json_array_append_new(updated, json_string(uid));
        }

        if (json_array_size(updated))
            json_object_set(set, "updated", updated);
        json_decref(updated);
        if (json_object_size(notUpdated))
            json_object_set(set, "notUpdated", notUpdated);
        json_decref(notUpdated);
    }

    json_t *destroy = json_object_get(req->args, "destroy");
    if (destroy) {
        json_t *destroyed = json_pack("[]");
        json_t *notDestroyed = json_pack("{}");

        size_t index;
        for (index = 0; index < json_array_size(destroy); index++) {
            const char *uid = _json_array_get_string(destroy, index);
            if (!uid) {
                json_t *err = json_pack("{s:s}", "type", "invalidArguments");
                json_object_set_new(notDestroyed, uid, err);
                continue;
            }
            struct carddav_data *cdata = NULL;
            uint32_t olduid;
            r = carddav_lookup_uid(db, uid, &cdata);

            /* is it a valid group? */
            if (r || !cdata ||
                !cdata->dav.imap_uid || cdata->kind != CARDDAV_KIND_GROUP) {
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "notFound");
                json_object_set_new(notDestroyed, uid, err);
                continue;
            }
            olduid = cdata->dav.imap_uid;

            if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(cdata->dav.mailbox, &mailbox);
                if (r) goto done;
            }

            /* XXX - alive check */

            syslog(LOG_NOTICE, "jmap: destroy group %s (%s)", req->userid, uid);
            r = carddav_remove(mailbox, olduid, /*isreplace*/0);
            if (r) {
                syslog(LOG_ERR,
                       "IOERROR: setContactGroups remove failed for %s %u",
                       mailbox->name, cdata->dav.imap_uid);
                goto done;
            }

            json_array_append_new(destroyed, json_string(uid));
        }

        if (json_array_size(destroyed))
            json_object_set(set, "destroyed", destroyed);
        json_decref(destroyed);
        if (json_object_size(notDestroyed))
            json_object_set(set, "notDestroyed", notDestroyed);
        json_decref(notDestroyed);
    }

    /* force modseq to stable */
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    /* read the modseq again every time, just in case something changed it
     * in our actions */
    struct buf buf = BUF_INITIALIZER;
    const char *inboxname = mboxname_user_mbox(req->userid, NULL);
    modseq_t modseq = mboxname_readmodseq(inboxname);
    buf_printf(&buf, "%llu", modseq);
    json_object_set_new(set, "newState", json_string(buf_cstring(&buf)));
    buf_free(&buf);

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contactGroupsSet"));
    json_array_append_new(item, set);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

done:
    mailbox_close(&newmailbox);
    mailbox_close(&mailbox);

    carddav_close(db);
    return r;
}

static int _wantprop(hash_table *props, const char *name)
{
    if (!props) return 1;
    if (hash_lookup(name, props)) return 1;
    return 0;
}

/* convert YYYY-MM-DD to separate y,m,d */
static int _parse_date(const char *date, unsigned *y, unsigned *m, unsigned *d)
{
    /* there isn't a convenient libc function that will let us convert parts of
     * a string to integer and only take digit characters, so we just pull it
     * apart ourselves */

    /* format check. no need to strlen() beforehand, it will fall out of this */
    if (date[0] < '0' || date[0] > '9' ||
        date[1] < '0' || date[1] > '9' ||
        date[2] < '0' || date[2] > '9' ||
        date[3] < '0' || date[3] > '9' ||
        date[4] != '-' ||
        date[5] < '0' || date[5] > '9' ||
        date[6] < '0' || date[6] > '9' ||
        date[7] != '-' ||
        date[8] < '0' || date[8] > '9' ||
        date[9] < '0' || date[9] > '9' ||
        date[10] != '\0')

        return -1;

    /* convert to integer. ascii digits are 0x30-0x37, so we can take bottom
     * four bits and multiply */
    *y =
        (date[0] & 0xf) * 1000 +
        (date[1] & 0xf) * 100 +
        (date[2] & 0xf) * 10 +
        (date[3] & 0xf);

    *m =
        (date[5] & 0xf) * 10 +
        (date[6] & 0xf);

    *d =
        (date[8] & 0xf) * 10 +
        (date[9] & 0xf);

    return 0;
}

static void _date_to_jmap(struct vparse_entry *entry, struct buf *buf)
{
    if (!entry)
        goto no_date;

    unsigned y, m, d;
    if (_parse_date(entry->v.value, &y, &m, &d))
        goto no_date;

    if (y < 1604 || m > 12 || d > 31)
        goto no_date;

    const struct vparse_param *param;
    for (param = entry->params; param; param = param->next) {
        if (!strcasecmp(param->name, "x-apple-omit-year"))
            /* XXX compare value with actual year? */
            y = 0;
        if (!strcasecmp(param->name, "x-fm-no-month"))
            m = 0;
        if (!strcasecmp(param->name, "x-fm-no-day"))
            d = 0;
    }

    /* sigh, magic year 1604 has been seen without X-APPLE-OMIT-YEAR, making
     * me wonder what the bloody point is */
    if (y == 1604)
        y = 0;

    buf_reset(buf);
    buf_printf(buf, "%04d-%02d-%02d", y, m, d);
    return;

no_date:
    buf_setcstr(buf, "0000-00-00");
}

static const char *_servicetype(const char *type)
{
    /* add new services here */
    if (!strcasecmp(type, "aim")) return "AIM";
    if (!strcasecmp(type, "facebook")) return "Facebook";
    if (!strcasecmp(type, "flickr")) return "Flickr";
    if (!strcasecmp(type, "gadugadu")) return "GaduGadu";
    if (!strcasecmp(type, "github")) return "GitHub";
    if (!strcasecmp(type, "googletalk")) return "GoogleTalk";
    if (!strcasecmp(type, "icq")) return "ICQ";
    if (!strcasecmp(type, "jabber")) return "Jabber";
    if (!strcasecmp(type, "linkedin")) return "LinkedIn";
    if (!strcasecmp(type, "msn")) return "MSN";
    if (!strcasecmp(type, "myspace")) return "MySpace";
    if (!strcasecmp(type, "qq")) return "QQ";
    if (!strcasecmp(type, "skype")) return "Skype";
    if (!strcasecmp(type, "twitter")) return "Twitter";
    if (!strcasecmp(type, "yahoo")) return "Yahoo";

    syslog(LOG_NOTICE, "unknown service type %s", type);
    return type;
}

static int getcontacts_cb(void *rock, struct carddav_data *cdata)
{
    struct cards_rock *crock = (struct cards_rock *) rock;
    struct index_record record;
    strarray_t *empty = NULL;
    int r = 0;

    if (crock->need) {
        /* skip records not in hash */
        if (!hash_lookup(cdata->vcard_uid, crock->need))
            return 0;
        /* mark 2 == seen */
        hash_insert(cdata->vcard_uid, (void *)2, crock->need);
    }

    if (!crock->mailbox || strcmp(crock->mailbox->name, cdata->dav.mailbox)) {
        mailbox_close(&crock->mailbox);
        r = mailbox_open_irl(cdata->dav.mailbox, &crock->mailbox);
        if (r) return r;
    }

    r = mailbox_find_index_record(crock->mailbox, cdata->dav.imap_uid, &record);
    if (r) return r;

    /* XXX - this could definitely be refactored from here and mailbox.c */
    struct buf msg_buf = BUF_INITIALIZER;
    struct vparse_state vparser;

    /* Load message containing the resource and parse vcard data */
    r = mailbox_map_record(crock->mailbox, &record, &msg_buf);
    if (r) return r;

    memset(&vparser, 0, sizeof(struct vparse_state));
    vparser.base = buf_cstring(&msg_buf) + record.header_size;
    vparse_set_multival(&vparser, "adr");
    vparse_set_multival(&vparser, "org");
    vparse_set_multival(&vparser, "n");
    r = vparse_parse(&vparser, 0);
    buf_free(&msg_buf);
    if (r) return r;
    if (!vparser.card || !vparser.card->objects) {
        vparse_free(&vparser);
        return r;
    }
    struct vparse_card *card = vparser.card->objects;

    json_t *obj = json_pack("{}");

    json_object_set_new(obj, "id", json_string(cdata->vcard_uid));

    json_object_set_new(obj, "addressbookId",
                        json_string(cdata->dav.mailbox + crock->mboxoffset));

    if (_wantprop(crock->props, "isFlagged")) {
        json_object_set_new(obj, "isFlagged",
                            record.system_flags & FLAG_FLAGGED ? json_true() :
                            json_false());
    }

    struct buf buf = BUF_INITIALIZER;

    if (_wantprop(crock->props, "x-href")) {
        _add_xhref(obj, cdata->dav.mailbox, cdata->dav.resource);
    }

    if (_wantprop(crock->props, "x-importance")) {
        double val = 0;
        const char *ns = DAV_ANNOT_NS "<" XML_NS_CYRUS ">importance";

        buf_reset(&buf);
        annotatemore_msg_lookup(crock->mailbox->name, record.uid,
                                ns, "", &buf);
        if (buf.len)
            val = strtod(buf_cstring(&buf), NULL);

        json_object_set_new(obj, "x-importance", json_real(val));
    }

    const strarray_t *n = vparse_multival(card, "n");
    const strarray_t *org = vparse_multival(card, "org");
    if (!n) n = empty ? empty : (empty = strarray_new());
    if (!org) org = empty ? empty : (empty = strarray_new());

    /* name fields: Family; Given; Middle; Prefix; Suffix. */

    if (_wantprop(crock->props, "lastName")) {
        const char *family = strarray_safenth(n, 0);
        const char *suffix = strarray_safenth(n, 4);
        buf_setcstr(&buf, family);
        if (*suffix) {
            buf_putc(&buf, ' ');
            buf_appendcstr(&buf, suffix);
        }
        json_object_set_new(obj, "lastName", json_string(buf_cstring(&buf)));
    }

    if (_wantprop(crock->props, "firstName")) {
        const char *given = strarray_safenth(n, 1);
        const char *middle = strarray_safenth(n, 2);
        buf_setcstr(&buf, given);
        if (*middle) {
            buf_putc(&buf, ' ');
            buf_appendcstr(&buf, middle);
        }
        json_object_set_new(obj, "firstName", json_string(buf_cstring(&buf)));
    }
    if (_wantprop(crock->props, "prefix")) {
        const char *prefix = strarray_safenth(n, 3);
        json_object_set_new(obj, "prefix",
                            json_string(prefix)); /* just prefix */
    }

    /* org fields */
    if (_wantprop(crock->props, "company"))
        json_object_set_new(obj, "company",
                            json_string(strarray_safenth(org, 0)));
    if (_wantprop(crock->props, "department"))
        json_object_set_new(obj, "department",
                            json_string(strarray_safenth(org, 1)));
    /* XXX - position? */

    /* address - we need to open code this, because it's repeated */
    if (_wantprop(crock->props, "addresses")) {
        json_t *adr = json_array();

        struct vparse_entry *entry;
        for (entry = card->properties; entry; entry = entry->next) {
            if (strcasecmp(entry->name, "adr")) continue;
            json_t *item = json_pack("{}");

            /* XXX - type and label */
            const strarray_t *a = entry->v.values;

            const struct vparse_param *param;
            const char *type = "other";
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "type")) {
                    if (!strcasecmp(param->value, "home")) {
                        type = "home";
                    }
                    else if (!strcasecmp(param->value, "work")) {
                        type = "work";
                    }
                    else if (!strcasecmp(param->value, "billing")) {
                        type = "billing";
                    }
                    else if (!strcasecmp(param->value, "postal")) {
                        type = "postal";
                    }
                }
                else if (!strcasecmp(param->name, "label")) {
                    label = param->value;
                }
            }
            json_object_set_new(item, "type", json_string(type));
            if (label) json_object_set_new(item, "label", json_string(label));

            const char *pobox = strarray_safenth(a, 0);
            const char *extended = strarray_safenth(a, 1);
            const char *street = strarray_safenth(a, 2);
            buf_reset(&buf);
            if (*pobox) {
                buf_appendcstr(&buf, pobox);
                if (extended || street) buf_putc(&buf, '\n');
            }
            if (*extended) {
                buf_appendcstr(&buf, extended);
                if (street) buf_putc(&buf, '\n');
            }
            if (*street) {
                buf_appendcstr(&buf, street);
            }

            json_object_set_new(item, "street",
                                json_string(buf_cstring(&buf)));
            json_object_set_new(item, "locality",
                                json_string(strarray_safenth(a, 3)));
            json_object_set_new(item, "region",
                                json_string(strarray_safenth(a, 4)));
            json_object_set_new(item, "postcode",
                                json_string(strarray_safenth(a, 5)));
            json_object_set_new(item, "country",
                                json_string(strarray_safenth(a, 6)));

            json_array_append_new(adr, item);
        }

        json_object_set_new(obj, "addresses", adr);
    }

    /* address - we need to open code this, because it's repeated */
    if (_wantprop(crock->props, "emails")) {
        json_t *emails = json_array();

        struct vparse_entry *entry;
        int defaultIndex = -1;
        int i = 0;
        for (entry = card->properties; entry; entry = entry->next) {
            if (strcasecmp(entry->name, "email")) continue;
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *type = "other";
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "type")) {
                    if (!strcasecmp(param->value, "home")) {
                        type = "personal";
                    }
                    else if (!strcasecmp(param->value, "work")) {
                        type = "work";
                    }
                    else if (!strcasecmp(param->value, "pref")) {
                        if (defaultIndex < 0)
                            defaultIndex = i;
                    }
                }
                else if (!strcasecmp(param->name, "label")) {
                    label = param->value;
                }
            }
            json_object_set_new(item, "type", json_string(type));
            if (label) json_object_set_new(item, "label", json_string(label));

            json_object_set_new(item, "value", json_string(entry->v.value));

            json_array_append_new(emails, item);
            i++;
        }

        if (defaultIndex < 0)
            defaultIndex = 0;
        int size = json_array_size(emails);
        for (i = 0; i < size; i++) {
            json_t *item = json_array_get(emails, i);
            json_object_set_new(item, "isDefault",
                                i == defaultIndex ? json_true() : json_false());
        }

        json_object_set_new(obj, "emails", emails);
    }

    /* address - we need to open code this, because it's repeated */
    if (_wantprop(crock->props, "phones")) {
        json_t *phones = json_array();

        struct vparse_entry *entry;
        for (entry = card->properties; entry; entry = entry->next) {
            if (strcasecmp(entry->name, "tel")) continue;
            json_t *item = json_pack("{}");
            const struct vparse_param *param;
            const char *type = "other";
            const char *label = NULL;
            for (param = entry->params; param; param = param->next) {
                if (!strcasecmp(param->name, "type")) {
                    if (!strcasecmp(param->value, "home")) {
                        type = "home";
                    }
                    else if (!strcasecmp(param->value, "work")) {
                        type = "work";
                    }
                    else if (!strcasecmp(param->value, "cell")) {
                        type = "mobile";
                    }
                    else if (!strcasecmp(param->value, "mobile")) {
                        type = "mobile";
                    }
                    else if (!strcasecmp(param->value, "fax")) {
                        type = "fax";
                    }
                    else if (!strcasecmp(param->value, "pager")) {
                        type = "pager";
                    }
                }
                else if (!strcasecmp(param->name, "label")) {
                    label = param->value;
                }
            }
            json_object_set_new(item, "type", json_string(type));
            if (label) json_object_set_new(item, "label", json_string(label));

            json_object_set_new(item, "value", json_string(entry->v.value));

            json_array_append_new(phones, item);
        }

        json_object_set_new(obj, "phones", phones);
    }

    /* address - we need to open code this, because it's repeated */
    if (_wantprop(crock->props, "online")) {
        json_t *online = json_array();

        struct vparse_entry *entry;
        for (entry = card->properties; entry; entry = entry->next) {
            if (!strcasecmp(entry->name, "url")) {
                json_t *item = json_pack("{}");
                const struct vparse_param *param;
                const char *label = NULL;
                for (param = entry->params; param; param = param->next) {
                    if (!strcasecmp(param->name, "label")) {
                        label = param->value;
                    }
                }
                json_object_set_new(item, "type", json_string("uri"));
                if (label) json_object_set_new(item, "label", json_string(label));
                json_object_set_new(item, "value", json_string(entry->v.value));
                json_array_append_new(online, item);
            }
            if (!strcasecmp(entry->name, "impp")) {
                json_t *item = json_pack("{}");
                const struct vparse_param *param;
                const char *label = NULL;
                for (param = entry->params; param; param = param->next) {
                    if (!strcasecmp(param->name, "x-service-type")) {
                        label = _servicetype(param->value);
                    }
                }
                json_object_set_new(item, "type", json_string("username"));
                if (label) json_object_set_new(item, "label", json_string(label));
                json_object_set_new(item, "value", json_string(entry->v.value));
                json_array_append_new(online, item);
            }
            if (!strcasecmp(entry->name, "x-social-profile")) {
                json_t *item = json_pack("{}");
                const struct vparse_param *param;
                const char *label = NULL;
                const char *value = NULL;
                for (param = entry->params; param; param = param->next) {
                    if (!strcasecmp(param->name, "type")) {
                        label = _servicetype(param->value);
                    }
                    if (!strcasecmp(param->name, "x-user")) {
                        value = param->value;
                    }
                }
                json_object_set_new(item, "type", json_string("username"));
                if (label) json_object_set_new(item, "label", json_string(label));
                json_object_set_new(item, "value",
                                    json_string(value ? value : entry->v.value));
                json_array_append_new(online, item);
            }
        }

        json_object_set_new(obj, "online", online);
    }

    if (_wantprop(crock->props, "nickname")) {
        const char *item = vparse_stringval(card, "nickname");
        json_object_set_new(obj, "nickname", json_string(item ? item : ""));
    }

    if (_wantprop(crock->props, "birthday")) {
        struct vparse_entry *entry = vparse_get_entry(card, NULL, "bday");
        _date_to_jmap(entry, &buf);
        json_object_set_new(obj, "birthday", json_string(buf_cstring(&buf)));
    }

    if (_wantprop(crock->props, "notes")) {
        const char *item = vparse_stringval(card, "note");
        json_object_set_new(obj, "notes", json_string(item ? item : ""));
    }

    if (_wantprop(crock->props, "x-hasPhoto")) {
        const char *item = vparse_stringval(card, "photo");
        json_object_set_new(obj, "x-hasPhoto",
                            item ? json_true() : json_false());
    }

    /* XXX - other fields */

    json_array_append_new(crock->array, obj);

    if (empty) strarray_free(empty);

    buf_free(&buf);

    return 0;
}

static int getContacts(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    const char *addressbookId = "Default";
    json_t *abookid = json_object_get(req->args, "addressbookId");
    if (abookid && json_string_value(abookid)) {
        /* XXX - invalid arguments */
        addressbookId = json_string_value(abookid);
    }
    const char *abookname = mboxname_abook(req->userid, addressbookId);

    struct cards_rock rock;
    int r;

    rock.array = json_pack("[]");
    rock.need = NULL;
    rock.props = NULL;
    rock.mailbox = NULL;
    rock.mboxoffset = strlen(abookname) - strlen(addressbookId);

    json_t *want = json_object_get(req->args, "ids");
    if (want) {
        rock.need = xzmalloc(sizeof(struct hash_table));
        construct_hash_table(rock.need, 1024, 0);
        int i;
        int size = json_array_size(want);
        for (i = 0; i < size; i++) {
            const char *id = json_string_value(json_array_get(want, i));
            if (id == NULL) {
                free_hash_table(rock.need, NULL);
                free(rock.need);
                return -1; /* XXX - need codes */
            }
            /* 1 == want */
            hash_insert(id, (void *)1, rock.need);
        }
    }

    json_t *properties = json_object_get(req->args, "properties");
    if (properties) {
        rock.props = xzmalloc(sizeof(struct hash_table));
        construct_hash_table(rock.props, 1024, 0);
        int i;
        int size = json_array_size(properties);
        for (i = 0; i < size; i++) {
            const char *id = json_string_value(json_array_get(properties, i));
            if (id == NULL) {
                free_hash_table(rock.need, NULL);
                free(rock.need);
                return -1; /* XXX - need codes */
            }
            /* 1 == properties */
            hash_insert(id, (void *)1, rock.props);
        }
    }

    r = carddav_get_cards(db, abookname, CARDDAV_KIND_CONTACT,
                          &getcontacts_cb, &rock);
    if (r) goto done;

    json_t *contacts = json_pack("{}");
    json_object_set_new(contacts, "accountId", json_string(req->userid));
    json_object_set_new(contacts, "state", json_string(req->state));
    json_object_set_new(contacts, "list", rock.array);
    if (rock.need) {
        json_t *notfound = json_array();
        hash_enumerate(rock.need, _add_notfound, notfound);
        free_hash_table(rock.need, NULL);
        free(rock.need);
        if (json_array_size(notfound)) {
            json_object_set_new(contacts, "notFound", notfound);
        }
        else {
            json_decref(notfound);
            json_object_set_new(contacts, "notFound", json_null());
        }
    }
    else {
        json_object_set_new(contacts, "notFound", json_null());
    }

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contacts"));
    json_array_append_new(item, contacts);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

  done:
    mailbox_close(&rock.mailbox);
    carddav_close(db);
    return r;
}

static int getContactUpdates(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    const char *since = _json_object_get_string(req->args, "sinceState");
    if (!since) return -1;
    modseq_t oldmodseq = str2uint64(since);

    struct updates_rock rock;
    rock.changed = json_array();
    rock.removed = json_array();

    int r = carddav_get_updates(db, oldmodseq, CARDDAV_KIND_CONTACT,
                                &getupdates_cb, &rock);
    if (r) goto done;

    strip_spurious_deletes(&rock);

    json_t *contactUpdates = json_pack("{}");
    json_object_set_new(contactUpdates, "accountId", json_string(req->userid));
    json_object_set_new(contactUpdates, "oldState", json_string(since));
    json_object_set_new(contactUpdates, "newState", json_string(req->state));
    json_object_set(contactUpdates, "changed", rock.changed);
    json_object_set(contactUpdates, "removed", rock.removed);

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contactUpdates"));
    json_array_append_new(item, contactUpdates);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

    json_t *dofetch = json_object_get(req->args, "fetchContacts");
    json_t *doprops = json_object_get(req->args, "fetchContactProperties");
    if (dofetch && json_is_true(dofetch) && json_array_size(rock.changed)) {
        struct jmap_req subreq = *req;
        subreq.args = json_pack("{}");
        json_object_set(subreq.args, "ids", rock.changed);
        if (doprops) json_object_set(subreq.args, "properties", doprops);
        json_t *abookid = json_object_get(req->args, "addressbookId");
        if (abookid) {
            json_object_set(subreq.args, "addressbookId", abookid);
        }
        r = getContacts(&subreq);
        json_decref(subreq.args);
    }

    json_decref(rock.changed);
    json_decref(rock.removed);

  done:
    carddav_close(db);
    return r;
}

static struct vparse_entry *_card_multi(struct vparse_card *card,
                                        const char *name)
{
    struct vparse_entry *res = vparse_get_entry(card, NULL, name);
    if (!res) {
        res = vparse_add_entry(card, NULL, name, NULL);
        res->multivalue = 1;
        res->v.values = strarray_new();
    }
    return res;
}

static int _emails_to_card(struct vparse_card *card, json_t *arg)
{
    vparse_delete_entries(card, NULL, "email");

    int i;
    int size = json_array_size(arg);
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        const char *type = _json_object_get_string(item, "type");
        if (!type) return -1;
        /*optional*/
        const char *label = _json_object_get_string(item, "label");
        const char *value = _json_object_get_string(item, "value");
        if (!value) return -1;
        json_t *jisDefault = json_object_get(item, "isDefault");

        struct vparse_entry *entry =
            vparse_add_entry(card, NULL, "email", value);

        if (!strcmpsafe(type, "personal"))
            type = "home";
        if (strcmpsafe(type, "other"))
            vparse_add_param(entry, "type", type);

        if (label)
            vparse_add_param(entry, "label", label);

        if (jisDefault && json_is_true(jisDefault))
            vparse_add_param(entry, "type", "pref");
    }
    return 0;
}

static int _phones_to_card(struct vparse_card *card, json_t *arg)
{
    vparse_delete_entries(card, NULL, "tel");

    int i;
    int size = json_array_size(arg);
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);
        const char *type = _json_object_get_string(item, "type");
        if (!type) return -1;
        /* optional */
        const char *label = _json_object_get_string(item, "label");
        const char *value = _json_object_get_string(item, "value");
        if (!value) return -1;

        struct vparse_entry *entry = vparse_add_entry(card, NULL, "tel", value);

        if (!strcmp(type, "mobile"))
            vparse_add_param(entry, "type", "cell");
        else if (strcmp(type, "other"))
            vparse_add_param(entry, "type", type);

        if (label)
            vparse_add_param(entry, "label", label);
    }
    return 0;
}

static int _is_im(const char *type)
{
    /* add new services here */
    if (!strcasecmp(type, "aim")) return 1;
    if (!strcasecmp(type, "facebook")) return 1;
    if (!strcasecmp(type, "gadugadu")) return 1;
    if (!strcasecmp(type, "googletalk")) return 1;
    if (!strcasecmp(type, "icq")) return 1;
    if (!strcasecmp(type, "jabber")) return 1;
    if (!strcasecmp(type, "msn")) return 1;
    if (!strcasecmp(type, "qq")) return 1;
    if (!strcasecmp(type, "skype")) return 1;
    if (!strcasecmp(type, "twitter")) return 1;
    if (!strcasecmp(type, "yahoo")) return 1;

    return 0;
}

static int _online_to_card(struct vparse_card *card, json_t *arg)
{
    vparse_delete_entries(card, NULL, "url");
    vparse_delete_entries(card, NULL, "impp");
    vparse_delete_entries(card, NULL, "x-social-profile");

    int i;
    int size = json_array_size(arg);
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);
        const char *value = _json_object_get_string(item, "value");
        if (!value) return -1;
        const char *type = _json_object_get_string(item, "type");
        if (!type) return -1;
        const char *label = _json_object_get_string(item, "label");

        if (!strcmp(type, "uri")) {
            struct vparse_entry *entry =
                vparse_add_entry(card, NULL, "url", value);
            if (label)
                vparse_add_param(entry, "label", label);
        }
        else if (!strcmp(type, "username")) {
            if (label && _is_im(label)) {
                struct vparse_entry *entry =
                    vparse_add_entry(card, NULL, "impp", value);
                vparse_add_param(entry, "x-type", label);
            }
            else {
                struct vparse_entry *entry =
                    vparse_add_entry(card, NULL, "x-social-profile", ""); // XXX - URL calculated, ick
                if (label)
                    vparse_add_param(entry, "type", label);
                vparse_add_param(entry, "x-user", value);
            }
        }
        /* XXX other? */
    }
    return 0;
}

static int _addresses_to_card(struct vparse_card *card, json_t *arg)
{
    vparse_delete_entries(card, NULL, "adr");

    int i;
    int size = json_array_size(arg);
    for (i = 0; i < size; i++) {
        json_t *item = json_array_get(arg, i);

        const char *type = _json_object_get_string(item, "type");
        if (!type) return -1;
        /* optional */
        const char *label = _json_object_get_string(item, "label");
        const char *street = _json_object_get_string(item, "street");
        if (!street) return -1;
        const char *locality = _json_object_get_string(item, "locality");
        if (!locality) return -1;
        const char *region = _json_object_get_string(item, "region");
        if (!region) return -1;
        const char *postcode = _json_object_get_string(item, "postcode");
        if (!postcode) return -1;
        const char *country = _json_object_get_string(item, "country");
        if (!country) return -1;

        struct vparse_entry *entry = vparse_add_entry(card, NULL, "adr", NULL);

        if (strcmpsafe(type, "other"))
            vparse_add_param(entry, "type", type);

        if (label)
            vparse_add_param(entry, "label", label);

        entry->multivalue = 1;
        entry->v.values = strarray_new();
        strarray_append(entry->v.values, ""); // PO Box
        strarray_append(entry->v.values, ""); // Extended Address
        strarray_append(entry->v.values, street);
        strarray_append(entry->v.values, locality);
        strarray_append(entry->v.values, region);
        strarray_append(entry->v.values, postcode);
        strarray_append(entry->v.values, country);
    }

    return 0;
}

static int _date_to_card(struct vparse_card *card,
                         const char *key, json_t *jval)
{
    if (!jval)
        return -1;
    const char *val = json_string_value(jval);
    if (!val)
        return -1;

    /* JMAP dates are always YYYY-MM-DD */
    unsigned y, m, d;
    if (_parse_date(val, &y, &m, &d))
        return -1;

    /* range checks. month and day just get basic sanity checks because we're
     * not carrying a full calendar implementation here. JMAP says zero is valid
     * so we'll allow that and deal with it later on */
    if (m > 12 || d > 31)
        return -1;

    /* all years are valid in JMAP, but ISO8601 only allows Gregorian ie >= 1583.
     * moreover, iOS uses 1604 as a magic number for "unknown", so we'll say 1605
     * is the minimum */
    if (y > 0 && y < 1605)
        return -1;

    /* everything in range. now comes the fun bit. vCard v3 says BDAY is
     * YYYY-MM-DD. It doesn't reference ISO8601 (vCard v4 does) and make no
     * provision for "unknown" date components, so there's no way to represent
     * JMAP's "unknown" values. Apple worked around this for year by using the
     * year 1604 and adding the parameter X-APPLE-OMIT-YEAR=1604 (value
     * apparently ignored). We will use a similar hack for month and day so we
     * can convert it back into a JMAP date */

    int no_year = 0;
    if (y == 0) {
        no_year = 1;
        y = 1604;
    }

    int no_month = 0;
    if (m == 0) {
        no_month = 1;
        m = 1;
    }

    int no_day = 0;
    if (d == 0) {
        no_day = 1;
        d = 1;
    }

    vparse_delete_entries(card, NULL, key);

    /* no values, we're done! */
    if (no_year && no_month && no_day)
        return 0;

    /* build the value */
    static char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    struct vparse_entry *entry = vparse_add_entry(card, NULL, key, buf);

    /* set all the round-trip flags, sigh */
    if (no_year)
        vparse_add_param(entry, "x-apple-omit-year", "1604");
    if (no_month)
        vparse_add_param(entry, "x-fm-no-month", "1");
    if (no_day)
        vparse_add_param(entry, "x-fm-no-day", "1");

    return 0;
}

static int _kv_to_card(struct vparse_card *card, const char *key, json_t *jval)
{
    if (!jval)
        return -1;
    const char *val = json_string_value(jval);
    if (!val)
        return -1;
    _card_val(card, key, val);
    return 0;
}

static void _make_fn(struct vparse_card *card)
{
    struct vparse_entry *n = vparse_get_entry(card, NULL, "n");
    strarray_t *name = strarray_new();
    const char *v;

    if (n) {
        v = strarray_safenth(n->v.values, 3); // prefix
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 1); // first
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 2); // middle
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 0); // last
        if (*v) strarray_append(name, v);

        v = strarray_safenth(n->v.values, 4); // suffix
        if (*v) strarray_append(name, v);
    }

    if (!strarray_size(name)) {
        v = vparse_stringval(card, "nickname");
        if (v && v[0]) strarray_append(name, v);
    }

    if (!strarray_size(name)) {
        /* XXX - grep type=pref?  Meh */
        v = vparse_stringval(card, "email");
        if (v && v[0]) strarray_append(name, v);
    }

    if (!strarray_size(name)) {
        strarray_append(name, "No Name");
    }

    char *fn = strarray_join(name, " ");

     _card_val(card, "fn", fn);
}

static int _json_to_card(struct vparse_card *card,
                         json_t *arg, strarray_t *flags,
                         struct entryattlist **annotsp)
{
    const char *key;
    json_t *jval;
    struct vparse_entry *fn = vparse_get_entry(card, NULL, "fn");
    int name_is_dirty = 0;
    int record_is_dirty = 0;
    /* we'll be updating you later anyway... create early so that it's
     * at the top of the card */
    if (!fn) {
        fn = vparse_add_entry(card, NULL, "fn", "No Name");
        name_is_dirty = 1;
    }

    json_object_foreach(arg, key, jval) {
        if (!strcmp(key, "isFlagged")) {
            if (json_is_true(jval)) {
                strarray_add_case(flags, "\\Flagged");
            }
            else {
                strarray_remove_all_case(flags, "\\Flagged");
            }
        }
        else if (!strcmp(key, "x-importance")) {
            double dval = json_number_value(jval);
            const char *ns = DAV_ANNOT_NS "<" XML_NS_CYRUS ">importance";
            const char *attrib = "value.shared";
            if (dval) {
                struct buf buf = BUF_INITIALIZER;
                buf_printf(&buf, "%e", dval);
                setentryatt(annotsp, ns, attrib, &buf);
                buf_free(&buf);
            }
            else {
                clearentryatt(annotsp, ns, attrib);
            }
        }
        else if (!strcmp(key, "avatar")) {
            /* XXX - file handling */
        }
        else if (!strcmp(key, "prefix")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            name_is_dirty = 1;
            struct vparse_entry *n = _card_multi(card, "n");
            strarray_set(n->v.values, 3, val);
        }
        else if (!strcmp(key, "firstName")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            name_is_dirty = 1;
            struct vparse_entry *n = _card_multi(card, "n");
            strarray_set(n->v.values, 1, val);
        }
        else if (!strcmp(key, "lastName")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            name_is_dirty = 1;
            struct vparse_entry *n = _card_multi(card, "n");
            strarray_set(n->v.values, 0, val);
        }
        else if (!strcmp(key, "suffix")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            name_is_dirty = 1;
            struct vparse_entry *n = _card_multi(card, "n");
            strarray_set(n->v.values, 4, val);
        }
        else if (!strcmp(key, "nickname")) {
            int r = _kv_to_card(card, "nickname", jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "birthday")) {
            int r = _date_to_card(card, "bday", jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "anniversary")) {
            int r = _kv_to_card(card, "anniversary", jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "company")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            struct vparse_entry *org = _card_multi(card, "org");
            strarray_set(org->v.values, 0, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "department")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            struct vparse_entry *org = _card_multi(card, "org");
            strarray_set(org->v.values, 1, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "jobTitle")) {
            const char *val = json_string_value(jval);
            if (!val)
                return -1;
            struct vparse_entry *org = _card_multi(card, "org");
            strarray_set(org->v.values, 2, val);
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "emails")) {
            int r = _emails_to_card(card, jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "phones")) {
            int r = _phones_to_card(card, jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "online")) {
            int r = _online_to_card(card, jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "addresses")) {
            int r = _addresses_to_card(card, jval);
            if (r) return r;
            record_is_dirty = 1;
        }
        else if (!strcmp(key, "notes")) {
            int r = _kv_to_card(card, "note", jval);
            if (r) return r;
            record_is_dirty = 1;
        }

        else {
            /* INVALID PARAM */
            return -1; /* XXX - need codes */
        }
    }

    if (name_is_dirty) {
        _make_fn(card);
        record_is_dirty = 1;
    }

    if (!record_is_dirty)
        return 204;  /* no content */

    return 0;
}

static int setContacts(struct jmap_req *req)
{
    struct carddav_db *db = carddav_open_userid(req->userid);
    if (!db) return -1;

    int r = 0;
    json_t *jcheckState = json_object_get(req->args, "ifInState");
    if (jcheckState) {
        const char *checkState = json_string_value(jcheckState);
        if (!checkState ||strcmp(req->state, checkState)) {
            json_t *item = json_pack("[s, {s:s}, s]",
                                     "error", "type", "stateMismatch",
                                     req->tag);
            json_array_append_new(req->response, item);
            return 0;
        }
    }
    json_t *set = json_pack("{s:s,s:s}",
                            "oldState", req->state,
                            "accountId", req->userid);

    struct mailbox *mailbox = NULL;
    struct mailbox *newmailbox = NULL;

    json_t *create = json_object_get(req->args, "create");
    if (create) {
        json_t *created = json_pack("{}");
        json_t *notCreated = json_pack("{}");
        json_t *record;

        const char *key;
        json_t *arg;
        json_object_foreach(create, key, arg) {
            const char *uid = makeuuid();
            strarray_t *flags = strarray_new();
            struct entryattlist *annots = NULL;

            const char *addressbookId = "Default";
            json_t *abookid = json_object_get(arg, "addressbookId");
            if (abookid && json_string_value(abookid)) {
                /* XXX - invalid arguments */
                addressbookId = json_string_value(abookid);
            }
            const char *mboxname = mboxname_abook(req->userid, addressbookId);
            json_object_del(arg, "addressbookId");
            addressbookId = NULL;

            struct vparse_card *card = vparse_new_card("VCARD");
            vparse_add_entry(card, NULL, "VERSION", "3.0");
            vparse_add_entry(card, NULL, "UID", uid);

            /* we need to create and append a record */
            if (!mailbox || strcmp(mailbox->name, mboxname)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(mboxname, &mailbox);
                if (r) {
                    vparse_free_card(card);
                    goto done;
                }
            }

            r = _json_to_card(card, arg, flags, &annots);
            if (r) {
                /* this is just a failure */
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "invalidParameters");
                json_object_set_new(notCreated, key, err);
                strarray_free(flags);
                freeentryatts(annots);
                vparse_free_card(card);
                continue;
            }

            syslog(LOG_NOTICE, "jmap: create contact %s/%s (%s)",
                   req->userid, addressbookId, uid);
            r = carddav_store(mailbox, card, NULL,
                              flags, annots, req->userid, req->authstate);
            vparse_free_card(card);
            strarray_free(flags);
            freeentryatts(annots);

            if (r) {
                goto done;
            }

            record = json_pack("{s:s}", "id", uid);
            json_object_set_new(created, key, record);

            /* hash_insert takes ownership of uid here, skanky I know */
            hash_insert(key, xstrdup(uid), req->idmap);
        }

        if (json_object_size(created))
            json_object_set(set, "created", created);
        json_decref(created);
        if (json_object_size(notCreated))
            json_object_set(set, "notCreated", notCreated);
        json_decref(notCreated);
    }

    json_t *update = json_object_get(req->args, "update");
    if (update) {
        json_t *updated = json_pack("[]");
        json_t *notUpdated = json_pack("{}");

        const char *uid;
        json_t *arg;
        json_object_foreach(update, uid, arg) {
            struct carddav_data *cdata = NULL;
            r = carddav_lookup_uid(db, uid, &cdata);
            uint32_t olduid;
            char *resource = NULL;

            if (r || !cdata || !cdata->dav.imap_uid
                  || cdata->kind != CARDDAV_KIND_CONTACT) {
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "notFound");
                json_object_set_new(notUpdated, uid, err);
                continue;
            }
            olduid = cdata->dav.imap_uid;
            resource = xstrdup(cdata->dav.resource);

            if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(cdata->dav.mailbox, &mailbox);
                if (r) {
                    syslog(LOG_ERR, "IOERROR: failed to open %s",
                           cdata->dav.mailbox);
                    goto done;
                }
            }

            json_t *abookid = json_object_get(arg, "addressbookId");
            if (abookid && json_string_value(abookid)) {
                const char *mboxname =
                    mboxname_abook(req->userid, json_string_value(abookid));
                if (strcmp(mboxname, cdata->dav.mailbox)) {
                    /* move */
                    r = mailbox_open_iwl(mboxname, &newmailbox);
                    if (r) {
                        syslog(LOG_ERR, "IOERROR: failed to open %s", mboxname);
                        goto done;
                    }
                }
                json_object_del(arg, "addressbookId");
            }

            /* XXX - this could definitely be refactored from here and mailbox.c */
            struct buf msg_buf = BUF_INITIALIZER;
            struct vparse_state vparser;
            struct index_record record;

            r = mailbox_find_index_record(mailbox, cdata->dav.imap_uid, &record);
            if (r) goto done;

            /* Load message containing the resource and parse vcard data */
            r = mailbox_map_record(mailbox, &record, &msg_buf);
            if (r) goto done;

            strarray_t *flags =
                mailbox_extract_flags(mailbox, &record, req->userid);
            struct entryattlist *annots =
                mailbox_extract_annots(mailbox, &record);

            memset(&vparser, 0, sizeof(struct vparse_state));
            vparser.base = buf_cstring(&msg_buf) + record.header_size;
            vparse_set_multival(&vparser, "adr");
            vparse_set_multival(&vparser, "org");
            vparse_set_multival(&vparser, "n");
            r = vparse_parse(&vparser, 0);
            buf_free(&msg_buf);
            if (r || !vparser.card || !vparser.card->objects) {
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "parseError");
                json_object_set_new(notUpdated, uid, err);
                vparse_free(&vparser);
                strarray_free(flags);
                freeentryatts(annots);
                mailbox_close(&newmailbox);
                free(resource);
                continue;
            }
            struct vparse_card *card = vparser.card->objects;

            r = _json_to_card(card, arg, flags, &annots);
            if (r == 204) {
                r = 0;
                if (!newmailbox) {
                    /* just bump the modseq
                       if in the same mailbox and no data change */
                    syslog(LOG_NOTICE, "jmap: touch contact %s/%s",
                           req->userid, resource);
                    if (strarray_find_case(flags, "\\Flagged", 0) >= 0)
                        record.system_flags |= FLAG_FLAGGED;
                    else
                        record.system_flags &= ~FLAG_FLAGGED;
                    annotate_state_t *state = NULL;
                    r = mailbox_get_annotate_state(mailbox, record.uid, &state);
                    annotate_state_set_auth(state, 0,
                                            req->userid, req->authstate);
                    if (!r) r = annotate_state_store(state, annots);
                    if (!r) r = mailbox_rewrite_index_record(mailbox, &record);
                    goto finish;
                }
            }
            if (r) {
                /* this is just a failure to create the JSON, not an error */
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "invalidParameters");
                json_object_set_new(notUpdated, uid, err);
                vparse_free(&vparser);
                strarray_free(flags);
                freeentryatts(annots);
                mailbox_close(&newmailbox);
                free(resource);
                continue;
            }

            syslog(LOG_NOTICE, "jmap: update contact %s/%s",
                   req->userid, resource);
            r = carddav_store(newmailbox ? newmailbox : mailbox, card, resource,
                              flags, annots, req->userid, req->authstate);
            if (!r)
                r = carddav_remove(mailbox, olduid, /*isreplace*/!newmailbox);

         finish:
            mailbox_close(&newmailbox);
            strarray_free(flags);
            freeentryatts(annots);

            vparse_free(&vparser);
            free(resource);

            if (r) goto done;

            json_array_append_new(updated, json_string(uid));
        }

        if (json_array_size(updated))
            json_object_set(set, "updated", updated);
        json_decref(updated);
        if (json_object_size(notUpdated))
            json_object_set(set, "notUpdated", notUpdated);
        json_decref(notUpdated);
    }

    json_t *destroy = json_object_get(req->args, "destroy");
    if (destroy) {
        json_t *destroyed = json_pack("[]");
        json_t *notDestroyed = json_pack("{}");

        size_t index;
        for (index = 0; index < json_array_size(destroy); index++) {
            const char *uid = _json_array_get_string(destroy, index);
            if (!uid) {
                json_t *err = json_pack("{s:s}", "type", "invalidArguments");
                json_object_set_new(notDestroyed, uid, err);
                continue;
            }
            struct carddav_data *cdata = NULL;
            uint32_t olduid;
            r = carddav_lookup_uid(db, uid, &cdata);

            if (r || !cdata || !cdata->dav.imap_uid
                  || cdata->kind != CARDDAV_KIND_CONTACT) {
                r = 0;
                json_t *err = json_pack("{s:s}", "type", "notFound");
                json_object_set_new(notDestroyed, uid, err);
                continue;
            }
            olduid = cdata->dav.imap_uid;

            if (!mailbox || strcmp(mailbox->name, cdata->dav.mailbox)) {
                mailbox_close(&mailbox);
                r = mailbox_open_iwl(cdata->dav.mailbox, &mailbox);
                if (r) goto done;
            }

            /* XXX - fricking mboxevent */

            syslog(LOG_NOTICE, "jmap: remove contact %s/%s", req->userid, uid);
            r = carddav_remove(mailbox, olduid, /*isreplace*/0);
            if (r) {
                syslog(LOG_ERR, "IOERROR: setContacts remove failed for %s %u",
                       mailbox->name, olduid);
                goto done;
            }

            json_array_append_new(destroyed, json_string(uid));
        }

        if (json_array_size(destroyed))
            json_object_set(set, "destroyed", destroyed);
        json_decref(destroyed);
        if (json_object_size(notDestroyed))
            json_object_set(set, "notDestroyed", notDestroyed);
        json_decref(notDestroyed);
    }

    /* force modseq to stable */
    if (mailbox) mailbox_unlock_index(mailbox, NULL);

    /* read the modseq again every time, just in case something changed it
     * in our actions */
    struct buf buf = BUF_INITIALIZER;
    const char *inboxname = mboxname_user_mbox(req->userid, NULL);
    modseq_t modseq = mboxname_readmodseq(inboxname);
    buf_printf(&buf, "%llu", modseq);
    json_object_set_new(set, "newState", json_string(buf_cstring(&buf)));
    buf_free(&buf);

    json_t *item = json_pack("[]");
    json_array_append_new(item, json_string("contactsSet"));
    json_array_append_new(item, set);
    json_array_append_new(item, json_string(req->tag));

    json_array_append_new(req->response, item);

done:
    mailbox_close(&newmailbox);
    mailbox_close(&mailbox);

    carddav_close(db);
    return r;
}