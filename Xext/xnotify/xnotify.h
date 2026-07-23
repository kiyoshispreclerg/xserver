/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * XNOTIFY extension - public interface: wire protocol structs, the per-action
 * permission bits, and the server-internal permission API. See xnotify.c for
 * the implementation and an overview of the permission model.
 */
#include "include/privates.h"
#include "dixstruct.h"

#define SERVER_XNOTIFY_MAJOR_VERSION		1
#define SERVER_XNOTIFY_MINOR_VERSION		0

#define X_XnotifyQueryVersion   0

typedef struct {
    CARD8   reqType;
    CARD8   xnotifyReqType;
    CARD16  length;
    CARD32  majorVersion;
    CARD32  minorVersion;
} xXnotifyQueryVersionReq;
#define sz_xXnotifyQueryVersionReq 12

typedef struct {
    BYTE    type;
    BYTE    pad1;
    CARD16  sequenceNumber;
    CARD32  length;
    CARD32  majorVersion;
    CARD32  minorVersion;
    CARD32  pad2;
    CARD32  pad3;
    CARD32  pad4;
    CARD32  pad5;
} xXnotifyQueryVersionReply;
#define sz_xXnotifyQueryVersionReply 32

/* Action bits: each sensitive capability the guard can allow or deny per client.
 * Values are 1-based bit positions (permission masks use 1U << (action - 1)). */
#define XNOTIFY_ATTACH          1   /* attach to shared memory (MIT-SHM) */
#define XNOTIFY_SELECTION       2   /* own or convert selections (clipboard) */
#define XNOTIFY_COMPOSITE       3   /* redirect / composite windows */
#define XNOTIFY_SCREEN          4   /* read screen contents (screenshots, GetImage) */
#define XNOTIFY_RECORD          5   /* record protocol/input via the RECORD extension */
#define XNOTIFY_CURSOR          6   /* read or change the cursor image */
#define XNOTIFY_INPUT_GRAB      7   /* actively grab the keyboard or pointer */
#define XNOTIFY_INPUT_INJECT    8   /* inject synthetic input (XTEST) */
#define XNOTIFY_HOTKEY          9   /* register global hotkeys (passive key grabs) */
#define XNOTIFY_INPUT           10  /* receive input events destined for other windows */
#define XNOTIFY_MANAGE          11  /* manage other clients' windows (configure, properties) */
#define XNOTIFY_GRAB_OVERRIDE   12  /* override an existing grab */
#define XNOTIFY_WARP            13  /* warp (move) the pointer */
#define XNOTIFY_FOCUS           14  /* change the input focus */
#define XNOTIFY_RANDR           15  /* change screen configuration (RandR) */
#define XNOTIFY_OVERLAY         16  /* access the composite overlay window */

#define XNOTIFY_MAX_ACTIONS     16                                /* highest per-client action bit */
#define XNOTIFY_ALL_ACTIONS_MASK ((1U << XNOTIFY_MAX_ACTIONS) - 1) /* mask of every action */
#define XNOTIFY_GUARD           17  /* authority to act as the permission manager (guard) */

#define MAX_PERM_ENTRIES        2048
#define HASH_TABLE_SIZE         512

#define MAX_ALLOWED_EXES        1024
#define ARGS_SIZE_MAX           512
#define EXE_PATH_MAX            512
#define XNOTIFY_THROTTLE_MS         1000
#define XNOTIFY_REPORT_THROTTLE_MS  2000
#define XNOTIFY_CACHE_MAX           1024
#define XNOTIFY_PENDING_THROTTLE_MS 3000

typedef struct XPermEntry {
    char               exe[EXE_PATH_MAX];
    uint32_t           perm_mask;
    char               args[ARGS_SIZE_MAX];
    struct XPermEntry *next;
    struct XPermEntry *next_list;
} XPermEntry;

extern XPermEntry *rule_list_head;
extern XPermEntry *perm_hash_table[HASH_TABLE_SIZE];
extern uint32_t    xnotify_global_allow_mask;

typedef struct {
    char *exe;
    char *args;
} ClientExeInfo;

void XnotifyInvalidateClientCache(ClientPtr client);
void XnotifyInvalidateAllClientCaches(void);
ClientExeInfo GetClientExeInfo(ClientPtr client);
void XnotifyReport(ClientPtr client, const int action);
void XnotifyAddPending(const char *exe, int action);
Bool XnotifyIsAllowed(ClientPtr client, const int action);
void XnotifyAllowExe(const int action, const char *exe_path, const char *args);
void XnotifyDenyExe(const int action, const char *exe_path);
void XnotifyAllowAction(const int action);
void XnotifyAllowAll(const char *exe_path, const char *args);
void XnotifyDenyAll(const char *exe_path);
void XnotifyClear(const int action);
int  XnotifyQueryAction(const int action, char out_exes[][EXE_PATH_MAX], int max_exes);
void XnotifyPoll(void);
void Xnotify(ClientPtr client, const int action);
void XnotifyLoadConfig(void);
char * GetClientExePath(ClientPtr client);
