#include <dix-config.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <poll.h>
#include <limits.h>
#include <dirent.h>

#include "xnotify.h"

#include "os/client_priv.h"
#include "os/auth.h"
#include "os/osdep.h"
#include "dix.h"
#include "privates.h"
#include "dix/request_priv.h"
#include "include/extnsionst.h"
#include "include/extension.h"
#include "miext/extinit_priv.h"

Bool        noXnotifyExtension = FALSE;

static Bool security_mode = FALSE;

static DevPrivateKeyRec xnotify_client_priv_key;

static inline uint32_t *
XnotifyPermMask(ClientPtr client)
{
    return dixLookupPrivate(&client->devPrivates, &xnotify_client_priv_key);
}

XPermEntry *rule_list_head = NULL;
XPermEntry *perm_hash_table[HASH_TABLE_SIZE] = {0};
uint32_t    xnotify_global_allow_mask = 0;

void
XnotifyInvalidateClientCache(ClientPtr client)
{
    (void)client;
}

void
XnotifyInvalidateAllClientCaches(void)
{
}

ClientExeInfo
GetClientExeInfo(ClientPtr client)
{
    (void)client;
    return (ClientExeInfo){ .exe = NULL, .args = NULL };
}

char *
GetClientExePath(ClientPtr client)
{
    (void)client;
    return NULL;
}

void
XnotifyReport(ClientPtr client, const int action)
{
    (void)client;
    (void)action;
}

Bool
XnotifyIsAllowed(ClientPtr client, const int action)
{
    (void)client;
    (void)action;
    return TRUE;
}

void
XnotifyAllowExe(const int action, const char *exe, const char *args)
{
    (void)action;
    (void)exe;
    (void)args;
}

void
XnotifyDenyExe(const int action, const char *exe)
{
    (void)action;
    (void)exe;
}

void
XnotifyAllowAll(const char *exe, const char *args)
{
    (void)exe;
    (void)args;
}

void
XnotifyDenyAll(const char *exe)
{
    (void)exe;
}

void
XnotifyAllowAction(const int action)
{
    (void)action;
}

void
XnotifyClear(const int action)
{
    (void)action;
}

int
XnotifyQueryAction(const int action, char out_exes[][EXE_PATH_MAX], int max_exes)
{
    (void)action;
    (void)out_exes;
    (void)max_exes;
    return 0;
}

void
XnotifyPoll(void)
{
}

void
Xnotify(ClientPtr client, const int action)
{
    (void)client;
    (void)action;
}

void
XnotifyLoadConfig(void)
{
}

static unsigned char XnotifyReqCode;

static int
ProcXnotifyQueryVersion(ClientPtr client)
{
    X_REQUEST_HEAD_STRUCT(xXnotifyQueryVersionReq);
    X_REQUEST_FIELD_CARD32(majorVersion);
    X_REQUEST_FIELD_CARD32(minorVersion);

    xXnotifyQueryVersionReply reply = {
        .majorVersion = stuff->majorVersion < SERVER_XNOTIFY_MAJOR_VERSION
                        ? stuff->majorVersion : SERVER_XNOTIFY_MAJOR_VERSION,
        .minorVersion = SERVER_XNOTIFY_MINOR_VERSION,
    };

    X_REPLY_FIELD_CARD32(majorVersion);
    X_REPLY_FIELD_CARD32(minorVersion);

    return X_SEND_REPLY_SIMPLE(client, reply);
}

static int
ProcXnotifyDispatch(ClientPtr client)
{
    REQUEST(xReq);

    switch (stuff->data) {
    case X_XnotifyQueryVersion:
        return ProcXnotifyQueryVersion(client);
    default:
        return BadRequest;
    }
}

void
XnotifyExtensionInit(void)
{
    if (!dixRegisterPrivateKey(&xnotify_client_priv_key, PRIVATE_CLIENT, sizeof(uint32_t)))
        FatalError("XnotifyExtensionInit: failed to register client private\n");

    ExtensionEntry *extEntry = AddExtension("XNOTIFY", 0, 0,
                                            ProcXnotifyDispatch,
                                            ProcXnotifyDispatch,
                                            NULL,
                                            StandardMinorOpcode);
    if (extEntry)
        XnotifyReqCode = (unsigned char) extEntry->base;
}
