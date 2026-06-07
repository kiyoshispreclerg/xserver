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
static XPermEntry *rule_list_tail = NULL;
XPermEntry *perm_hash_table[HASH_TABLE_SIZE] = {0};
uint32_t    xnotify_global_allow_mask = 0;

typedef struct {
    char  exe[EXE_PATH_MAX];
    int   action;
    Time  last_time;
} XPendingEntry;

static XPendingEntry xpending_cache[XNOTIFY_CACHE_MAX] = {0};
static int           xpending_count = 0;

static Bool pattern_matches(const char *exe, const char *args, const char *rule_exe, const char *rule_args);
static int  action_from_string(const char *s);
static int  XnotifyLoadFile(const char *filename);
static int  XnotifyLoadConfigDir(const char *dir_path);

static unsigned int
hash_exe(const char *str1, const char *str2) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str1++))
        hash = ((hash << 5) + hash) + c;  /* djb2 */
    if (str2)
        while ((c = *str2++))
            hash = ((hash << 5) + hash) + c;
    return hash % HASH_TABLE_SIZE;
}

static XPermEntry *
find_exact_entry(const char *exe) {
    if (!exe || !*exe)
        return NULL;

    unsigned int h = hash_exe(exe, NULL);
    XPermEntry *entry = perm_hash_table[h];

    while (entry) {
        if (strcmp(entry->exe, exe) == 0)
            return entry;
        entry = entry->next;
    }
    return NULL;
}

static XPermEntry *
find_matching_entry(const ClientExeInfo *info) {
    if (!info || !info->exe || !*info->exe)
        return NULL;

    XPermEntry *entry = find_exact_entry(info->exe);
    if (entry)
        return entry;

    for (XPermEntry *curr = rule_list_head; curr != NULL; curr = curr->next_list) {
        if (pattern_matches(info->exe, info->args, curr->exe, curr->args[0] ? curr->args : NULL)) {
            return curr;
        }
    }

    return NULL;
}

static XPermEntry *
create_or_get_entry(const char *exe, const char *args) {
    XPermEntry *existing = find_exact_entry(exe);
    if (existing)
        return existing;

    unsigned int h = hash_exe(exe, args);
    XPermEntry *new_entry = calloc(1, sizeof(XPermEntry));
    if (!new_entry) return NULL;

    strncpy(new_entry->exe, exe, EXE_PATH_MAX-1);
    new_entry->exe[EXE_PATH_MAX-1] = '\0';
    new_entry->perm_mask = 0;

    if (args) {
        strncpy(new_entry->args, args, ARGS_SIZE_MAX-1);
        new_entry->args[ARGS_SIZE_MAX-1] = '\0';
    } else {
        new_entry->args[0] = '\0';
    }

    new_entry->next = perm_hash_table[h];
    perm_hash_table[h] = new_entry;

    if (rule_list_head == NULL) {
        rule_list_head = rule_list_tail = new_entry;
    } else {
        rule_list_tail->next_list = new_entry;
        rule_list_tail = new_entry;
    }

    return new_entry;
}

static Bool
pattern_matches(const char *exe, const char *args, const char *rule_exe, const char *rule_args) {
    if (!exe || !rule_exe)
        return FALSE;

    char rule_key[EXE_PATH_MAX + ARGS_SIZE_MAX + 16] = {0};
    char client_key[EXE_PATH_MAX + ARGS_SIZE_MAX + 16] = {0};

    if (rule_args && *rule_args) {
        snprintf(client_key, sizeof(client_key), "%s|%s",
                 exe, args ? args : "");

        snprintf(rule_key, sizeof(rule_key), "%s|%s",
                 rule_exe, rule_args);
    }
    else {
        strncpy(client_key, exe, sizeof(client_key)-1);
        strncpy(rule_key, rule_exe, sizeof(rule_key)-1);
    }

    const char *p = rule_key;
    const char *s = client_key;
    const char *star = NULL;
    const char *ss = NULL;

    while (*s) {
        if (*p == '*') {
            star = p++;
            ss = s;
        } else if (*p == *s) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else
            return FALSE;
    }

    while (*p == '*')
        p++;

    return *p == '\0';
}

static int
action_from_string(const char *s) {
    if (!s || !*s) return 0;
    if (strcasecmp(s, "ALL") == 0) return -1;

    static const struct { const char *name; int val; } map[] = {
        {"ATTACH",        XNOTIFY_ATTACH},
        {"SELECTION",     XNOTIFY_SELECTION},
        {"COMPOSITE",     XNOTIFY_COMPOSITE},
        {"SCREEN",        XNOTIFY_SCREEN},
        {"RECORD",        XNOTIFY_RECORD},
        {"CURSOR",        XNOTIFY_CURSOR},
        {"INPUT_GRAB",    XNOTIFY_INPUT_GRAB},
        {"INPUT_INJECT",  XNOTIFY_INPUT_INJECT},
        {"HOTKEY",        XNOTIFY_HOTKEY},
        {"INPUT",         XNOTIFY_INPUT},
        {"MANAGE",        XNOTIFY_MANAGE},
        {"GRAB_OVERRIDE", XNOTIFY_GRAB_OVERRIDE},
        {"XNOTIFY",       XNOTIFY_GUARD},
        {NULL, 0}
    };

    for (int i = 0; map[i].name; i++)
        if (strcasecmp(s, map[i].name) == 0)
            return map[i].val;

    return atoi(s);
}

static int
XnotifyLoadFile(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f)
        return 0;

    char line[EXE_PATH_MAX + 128];
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\n')
            continue;

        char *cmd = strtok(p, " \t\r\n");
        if (!cmd)
            continue;

        if (strcasecmp(cmd, "ALLOW") == 0 || strcasecmp(cmd, "DENY") == 0) {
            Bool is_allow = (strcasecmp(cmd, "ALLOW") == 0);

            char *token1 = strtok(NULL, " \t\r\n");
            if (!token1)
                continue;

            int no_action = (token1[0] == '/');

            int action = no_action ? -1 : action_from_string(token1);
            char *token2 = no_action ? token1 : strtok(NULL, "|\t\r\n");
            char *token3 = no_action ? NULL  : strtok(NULL, "\t\r\n");

            if (no_action && token2) {
                char *pipe = strchr(token2, '|');
                if (pipe) {
                    *pipe = '\0';
                    token3 = pipe + 1;
                    char *end = token3 + strlen(token3) - 1;
                    while (end >= token3 && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t'))
                        *end-- = '\0';
                    if (*token3 == '\0')
                        token3 = NULL;
                }
            }

            if (token3 && strlen(token3) >= ARGS_SIZE_MAX)
                token3[0] = '\0';

            if (action == -1) {
                if (token2) {
                    if (is_allow)
                        XnotifyAllowAll(token2, token3);
                    else
                        XnotifyDenyAll(token2);
                    loaded++;
                }
            } else if (action > 0) {
                if (token2) {
                    if (is_allow)
                        XnotifyAllowExe(action, token2, token3);
                    else
                        XnotifyDenyExe(action, token2);
                    loaded++;
                } else {
                    if (is_allow)
                        XnotifyAllowAction(action);
                    else {
                        XnotifyClear(action);
                    }
                    loaded++;
                }
            }
        } else if (strcasecmp(cmd, "ALLOW_ACTION") == 0) {
            char *act_str = strtok(NULL, " \t\r\n");
            if (act_str) {
                int action = action_from_string(act_str);
                if (action > 0) {
                    XnotifyAllowAction(action);
                    loaded++;
                }
            }
        }
    }
    fclose(f);
    return loaded;
}

void
XnotifyLoadConfig(void) {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        XPermEntry *entry = perm_hash_table[i];
        while (entry) {
            XPermEntry *next = entry->next;
            free(entry);
            entry = next;
        }
        perm_hash_table[i] = NULL;
    }
    rule_list_head = NULL;
    rule_list_tail = NULL;
    xnotify_global_allow_mask = 0;

    int total = 0;
    total += XnotifyLoadConfigDir(SYSCONFDIR "/xnotify.conf.d");
    total += XnotifyLoadFile(SYSCONFDIR "/xnotify.conf");

    if (total > 0) {
        security_mode = TRUE;
        ErrorF("Xnotify: loaded %d permissions from %s (security mode enabled)\n", total, SYSCONFDIR);
    }
    else {
        security_mode = FALSE;
        ErrorF("Xnotify: no config found in %s - legacy mode (allow all)\n", SYSCONFDIR);
    }
}

static int
XnotifyLoadConfigDir(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;

    char **files = NULL;
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN)
            continue;
        size_t len = strlen(entry->d_name);
        if (len <= 5 || strcasecmp(entry->d_name + len - 5, ".conf") != 0)
            continue;

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);

        char **tmp = realloc(files, (count + 1) * sizeof(char*));
        if (!tmp) {
            free(files);
            closedir(dir);
            return 0;
        }
        files = tmp;
        files[count] = strdup(full);
        if (!files[count]) {
            for (int i = 0; i < count; i++)
                free(files[i]);
            free(files);
            closedir(dir);
            return 0;
        }
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free(files);
        return 0;
    }

    qsort(files, count, sizeof(char*), (int (*)(const void*, const void*))strcmp);

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        ErrorF("Xnotify: loading drop-in %s\n", files[i]);
        loaded += XnotifyLoadFile(files[i]);
        free(files[i]);
    }
    free(files);
    return loaded;
}

void
XnotifyInvalidateClientCache(ClientPtr client) {
    if (client)
        *XnotifyPermMask(client) = 0;
}

void
XnotifyInvalidateAllClientCaches(void) {
    for (int i = 0; i < currentMaxClients; i++) {
        if (clients[i])
            *XnotifyPermMask(clients[i]) = 0;
    }
}

void
XnotifyAddPending(const char *exe, int action) {
    if (!exe || action <= 0)
        return;

    Time now = GetTimeInMillis();

    for (int i = 0; i < xpending_count; i++) {
        if (strcmp(xpending_cache[i].exe, exe) == 0 && xpending_cache[i].action == action) {
            xpending_cache[i].last_time = now;
            return;
        }
    }

    if (xpending_count < XNOTIFY_CACHE_MAX) {
        strncpy(xpending_cache[xpending_count].exe, exe, EXE_PATH_MAX-1);
        xpending_cache[xpending_count].exe[EXE_PATH_MAX-1] = '\0';
        xpending_cache[xpending_count].action = action;
        xpending_cache[xpending_count].last_time = now;
        xpending_count++;
    }
}

static Bool
XnotifyHasRecentPending(const char *exe, int action) {
    if (!exe || action <= 0)
        return FALSE;

    Time now = GetTimeInMillis();

    for (int i = 0; i < xpending_count; i++) {
        if (strcmp(xpending_cache[i].exe, exe) == 0 &&
            xpending_cache[i].action == action) {

            if (now - xpending_cache[i].last_time < XNOTIFY_PENDING_THROTTLE_MS)
                return TRUE;

            for (int j = i; j < xpending_count - 1; j++)
                xpending_cache[j] = xpending_cache[j + 1];
            xpending_count--;
            return FALSE;
        }
    }
    return FALSE;
}

static void
XnotifyRemovePending(const char *exe, int action) {
    if (!exe || action <= 0)
        return;

    for (int i = 0; i < xpending_count; ) {
        if (strcmp(xpending_cache[i].exe, exe) == 0) {
            if (action == 0 || xpending_cache[i].action == action) {
                for (int j = i; j < xpending_count - 1; j++)
                    xpending_cache[j] = xpending_cache[j + 1];
                xpending_count--;
                continue;
            }
        }
        i++;
    }
}

void
XnotifyAllowExe(const int action, const char *exe, const char *args) {
    if (action == 0 || !exe || *exe == '\0')
        return;

    XPermEntry *entry = create_or_get_entry(exe, args);
    if (entry) {
        entry->perm_mask |= (1U << (action - 1));
        XnotifyRemovePending(exe, action);
    }
}

void
XnotifyDenyExe(const int action, const char *exe) {
    if (action == 0 || !exe || !*exe)
        return;

    uint32_t bit = (1U << (action - 1));

    ClientExeInfo info = { .exe = (char *)exe, .args = NULL };

    XPermEntry *entry = find_matching_entry(&info);
    if (entry) {
        entry->perm_mask &= ~bit;
    }

    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        XPermEntry *curr = perm_hash_table[i];
        while (curr) {
            if (pattern_matches(exe, NULL, curr->exe, curr->args)) {
                curr->perm_mask &= ~bit;
            }
            curr = curr->next;
        }
    }
}

void
XnotifyAllowAll(const char *exe, const char *args) {
    if (!exe || *exe == '\0')
        return;

    XPermEntry *entry = create_or_get_entry(exe, args);
    if (entry) {
        entry->perm_mask = XNOTIFY_ALL_ACTIONS_MASK;
        XnotifyRemovePending(exe, 0);
    }
}

void
XnotifyDenyAll(const char *exe) {
    if (!exe || !*exe)
        return;

    XPermEntry *entry = find_exact_entry(exe);
    if (entry) {
        entry->perm_mask = 0;
    }
}

void
XnotifyAllowAction(const int action) {
    if (action > 0 && action <= XNOTIFY_MAX_ACTIONS)
        xnotify_global_allow_mask |= (1U << (action - 1));

    for (int i = 0; i < xpending_count; ) {
        if (xpending_cache[i].action == action) {
            for (int j = i; j < xpending_count - 1; j++)
                xpending_cache[j] = xpending_cache[j + 1];
            xpending_count--;
            continue;
        }
        i++;
    }
}

int
XnotifyQueryAction(const int action, char out_exes[][EXE_PATH_MAX], int max_exes) {
    if (action == 0 || !out_exes || max_exes <= 0)
        return 0;

    if (xnotify_global_allow_mask & (1U << (action - 1)))
        return -1;

    int count = 0;
    for (int i = 0; i < HASH_TABLE_SIZE && count < max_exes; i++) {
        XPermEntry *entry = perm_hash_table[i];
        while (entry && count < max_exes) {
            if (entry->perm_mask & (1U << (action - 1))) {
                strlcpy(out_exes[count], entry->exe, EXE_PATH_MAX);
                count++;
            }
            entry = entry->next;
        }
    }
    return count;
}

void
XnotifyClear(const int action) {
    if (action <= 0) return;

    uint32_t bit = (1U << (action - 1));
    xnotify_global_allow_mask &= ~bit;

    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        XPermEntry *entry = perm_hash_table[i];
        while (entry) {
            entry->perm_mask &= ~bit;
            entry = entry->next;
        }
    }
}

typedef struct {
    pid_t pid;
    char  exe[EXE_PATH_MAX];
    Time  last_used;
} XnotifyCacheEntry;

static XnotifyCacheEntry xnotify_cache[XNOTIFY_CACHE_MAX] = {0};
static int               xnotify_cache_count = 0;
static Time              xnotify_last_cleanup = 0;

static void
XnotifyCacheCleanup(void) {
    Time now = GetTimeInMillis();
    if (now - xnotify_last_cleanup < 30000)
        return;

    xnotify_last_cleanup = now;

    int write_idx = 0;
    for (int i = 0; i < xnotify_cache_count; i++) {
        if (kill(xnotify_cache[i].pid, 0) == 0 || errno == EPERM) {
            if (write_idx != i)
                xnotify_cache[write_idx] = xnotify_cache[i];
            write_idx++;
        }
    }
    xnotify_cache_count = write_idx;

    write_idx = 0;
    for (int i = 0; i < xpending_count; i++) {
        if (now - xpending_cache[i].last_time < XNOTIFY_PENDING_THROTTLE_MS) {
            if (write_idx != i)
                xpending_cache[write_idx] = xpending_cache[i];
            write_idx++;
        }
    }
    xpending_count = write_idx;
}

static char *
XnotifyGetExe(pid_t pid) {
    if (pid <= 0)
        return NULL;

    Time now = GetTimeInMillis();

    for (int i = 0; i < xnotify_cache_count; i++) {
        if (xnotify_cache[i].pid == pid) {
            xnotify_cache[i].last_used = now;
            return xnotify_cache[i].exe;
        }
    }

    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%d/exe", pid);

    char path[EXE_PATH_MAX];
    ssize_t len = readlink(proc, path, sizeof(path)-1);
    if (len > 0) {
        path[len] = '\0';

        char *deleted = strstr(path, " (deleted)");
        if (deleted)
            *deleted = '\0';
    }

    if (len <= 0 || path[0] == '\0') {
        char proc_cmdline[64];
        snprintf(proc_cmdline, sizeof(proc_cmdline), "/proc/%d/cmdline", pid);

        int fd = open(proc_cmdline, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, path, sizeof(path)-1);
            close(fd);
            if (n > 0)
                path[n] = '\0';
        }
    }

    if (path[0] == '\0')
        return NULL;

    if (xnotify_cache_count < XNOTIFY_CACHE_MAX) {
        xnotify_cache[xnotify_cache_count].pid = pid;
        strlcpy(xnotify_cache[xnotify_cache_count].exe, path, EXE_PATH_MAX);
        xnotify_cache[xnotify_cache_count].last_used = now;
        xnotify_cache_count++;
        return xnotify_cache[xnotify_cache_count-1].exe;
    } else {
        int oldest = 0;
        for (int i = 1; i < XNOTIFY_CACHE_MAX; i++) {
            if (xnotify_cache[i].last_used < xnotify_cache[oldest].last_used)
                oldest = i;
        }
        xnotify_cache[oldest].pid = pid;
        strlcpy(xnotify_cache[oldest].exe, path, EXE_PATH_MAX);
        xnotify_cache[oldest].last_used = now;
        return xnotify_cache[oldest].exe;
    }
}

ClientExeInfo
GetClientExeInfo(ClientPtr client) {
    ClientExeInfo info = { .exe = NULL, .args = NULL };

    pid_t pid = GetClientPid(client);
    if (pid <= 0)
        return info;

    char *exe = XnotifyGetExe(pid);
    if (!exe)
        return info;

    info.exe = strdup(exe);

    const char *args = GetClientCmdArgs(client);
    if (args && *args)
        info.args = strdup(args);

    return info;
}

char *
GetClientExePath(ClientPtr client)
{
    pid_t pid = GetClientPid(client);
    if (pid <= 0)
        return NULL;
    return XnotifyGetExe(pid);
}

static int notify_sock = -1;
static int command_sock = -1;
static OsTimerPtr command_timer = NULL;

static Bool guard_active = FALSE;
static Time last_guard_heartbeat = 0;
#define GUARD_HEARTBEAT_TIMEOUT  5000
static pid_t pid_guard = 0;

static Bool xnotify_enabled = TRUE;

static void XnotifyInitCommand(void);
static Bool XnotifyIsGuardAlive(void);
static CARD32 XnotifyTimerCallback(OsTimerPtr timer, CARD32 now, void *arg);

static void
XnotifyGuardHeartbeat(pid_t pid) {
    if (!security_mode)
        return;

    last_guard_heartbeat = GetTimeInMillis();
    if (!guard_active) {
        guard_active = TRUE;
        pid_guard = pid;
        ErrorF("Xnotify: permission manager connected\n");
    }
}

static void
XnotifyClearAllDynamicPermissions(void) {
    XnotifyInvalidateAllClientCaches();

    XPermEntry *curr = rule_list_head;
    while (curr) {
        XPermEntry *next = curr->next_list;
        free(curr);
        curr = next;
    }
    rule_list_head = NULL;
    rule_list_tail = NULL;

    memset(perm_hash_table, 0, sizeof(perm_hash_table));

    xnotify_global_allow_mask = 0;

    xnotify_cache_count = 0;
    xpending_count = 0;
    xnotify_last_cleanup = 0;
}

static Bool
XnotifyIsGuardAlive(void) {
    if (!guard_active)
        return FALSE;

    Time now = GetTimeInMillis();
    if (now - last_guard_heartbeat > GUARD_HEARTBEAT_TIMEOUT) {
        ErrorF("Xnotify: permission manager disconnected since %d ms - resetting to system permissions\n",
               GUARD_HEARTBEAT_TIMEOUT);

        XnotifyClearAllDynamicPermissions();
        guard_active = FALSE;
        pid_guard = 0;
        XnotifyLoadConfig();
        return FALSE;
    }
    return TRUE;
}

static const char *
XnotifyGetPermSocket(void) {
    static char path[256] = {0};

    if (path[0] != '\0')
        return path;

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *display = dixGetDisplayName(NULL);
    if (runtime_dir && *runtime_dir) {
        snprintf(path, sizeof(path), "%s/xperms.%s.sock", runtime_dir, display);
    } else {
        snprintf(path, sizeof(path), "/tmp/xperms.%s.sock", display);
    }

    return path;
}

static const char *
XnotifyGetSocket(void) {
    static char path[256] = {0};

    if (path[0] != '\0')
        return path;

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *display = dixGetDisplayName(NULL);
    if (runtime_dir && *runtime_dir) {
        snprintf(path, sizeof(path), "%s/xnotify.%s.sock", runtime_dir, display);
    } else {
        snprintf(path, sizeof(path), "/tmp/xnotify.%s.sock", display);
    }

    return path;
}

static Bool
XnotifyResolveExeStrict(pid_t pid, char *out, size_t outlen) {
    if (pid <= 0 || outlen == 0)
        return FALSE;

    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/%d/exe", (int)pid);

    ssize_t n = readlink(proc, out, outlen - 1);
    if (n <= 0)
        return FALSE;
    out[n] = '\0';

    if (strstr(out, " (deleted)"))
        return FALSE;

    return TRUE;
}

static Bool
XnotifyExeIsGuard(const char *exe) {
    if (!exe || !*exe)
        return FALSE;

    uint32_t guard_bit = (1U << (XNOTIFY_GUARD - 1));
    for (XPermEntry *curr = rule_list_head; curr != NULL; curr = curr->next_list) {
        if (!(curr->perm_mask & guard_bit))
            continue;

        if (pattern_matches(exe, NULL, curr->exe, curr->args[0] ? curr->args : NULL))
            return TRUE;
    }
    return FALSE;
}

static Bool
is_trusted_sender(Bool have_cred, uid_t peer_uid, pid_t peer_pid) {
    if (!security_mode)
        return FALSE;

#ifdef __linux__
    if (!have_cred) {
        ErrorF("Xnotify: rejecting message without peer credentials\n");
        return FALSE;
    }
    uid_t server_uid = geteuid();
    if (peer_uid != server_uid && peer_uid != 0) {
        ErrorF("Xnotify: rejecting message from uid %u (expected %u or root)\n",
               (unsigned)peer_uid, (unsigned)server_uid);
        return FALSE;
    }
#else
    (void)have_cred;
    (void)peer_uid;
#endif

    char exe[EXE_PATH_MAX];
    if (!XnotifyResolveExeStrict(peer_pid, exe, sizeof(exe))) {
        ErrorF("Xnotify: rejecting message - cannot resolve sender exe (pid %d)\n",
               (int)peer_pid);
        return FALSE;
    }

    if (!XnotifyExeIsGuard(exe)) {
        ErrorF("Xnotify: rejecting message - %s is not an authorized guard\n", exe);
        return FALSE;
    }

    return TRUE;
}

static ssize_t
XnotifyRecvCommand(int fd, char *buf, size_t buflen,
                   Bool *have_cred, uid_t *peer_uid, pid_t *peer_pid) {
    *have_cred = FALSE;
    *peer_uid  = (uid_t)-1;
    *peer_pid  = -1;

    struct iovec iov = { .iov_base = buf, .iov_len = buflen };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };

#ifdef __linux__
    union {
        char buf[CMSG_SPACE(sizeof(struct ucred))];
        struct cmsghdr align;
    } cmsgbuf;
    msg.msg_control = cmsgbuf.buf;
    msg.msg_controllen = sizeof(cmsgbuf.buf);
#endif

    ssize_t n = recvmsg(fd, &msg, 0);
    if (n <= 0)
        return n;

#ifdef __linux__
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type  == SCM_CREDENTIALS &&
            cmsg->cmsg_len   == CMSG_LEN(sizeof(struct ucred))) {
            struct ucred uc;
            memcpy(&uc, CMSG_DATA(cmsg), sizeof(uc));
            *peer_uid  = uc.uid;
            *peer_pid  = uc.pid;
            *have_cred = TRUE;
        }
    }
#endif
    return n;
}

static void
XnotifyInit(void) {
    if (notify_sock != -1)
        return;

    notify_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (notify_sock == -1)
        return;

    fcntl(notify_sock, F_SETFL, O_NONBLOCK);

    const char *socket_path = XnotifyGetSocket();

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));

    if (connect(notify_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        close(notify_sock);
        notify_sock = -1;
        return;
    }
}

static void
XnotifyInitCommand(void) {
    if (command_sock != -1)
        return;

    XnotifyLoadConfig();

    const char *socket_path = XnotifyGetPermSocket();
    unlink(socket_path);

    command_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (command_sock == -1)
        return;

    fcntl(command_sock, F_SETFL, O_NONBLOCK);

#ifdef __linux__
    int passcred = 1;
    if (setsockopt(command_sock, SOL_SOCKET, SO_PASSCRED,
                   &passcred, sizeof(passcred)) == -1)
        ErrorF("Xnotify: SO_PASSCRED failed: %s\n", strerror(errno));
#endif

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, socket_path, sizeof(addr.sun_path));

    if (bind(command_sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        ErrorF("Xnotify: bind failed %s: %s\n", socket_path, strerror(errno));
        close(command_sock);
        command_sock = -1;
        return;
    }

    chmod(socket_path, 0600);
    ErrorF("Xnotify: socket ready at %s (waiting for permission manager)\n", socket_path);

    if (command_timer == NULL) {
        command_timer = TimerSet(command_timer, 0, 350, XnotifyTimerCallback, NULL);
        if (command_timer == NULL)
            ErrorF("Xnotify: failed to create timer\n");
    }
}

static CARD32
XnotifyTimerCallback(OsTimerPtr timer, CARD32 now, void *arg) {
    (void)timer; (void)now; (void)arg;
    command_timer = TimerSet(command_timer, 0, 500, XnotifyTimerCallback, NULL);
    if (command_timer == NULL)
        ErrorF("Xnotify: failed to recreate timer\n");

    XnotifyCacheCleanup();
    XnotifyPoll();
    return 0;
}

static void
XnotifySendQueryResponse(const int action, char exes[][EXE_PATH_MAX], int count) {
    if (notify_sock == -1)
        return;

    char msg[2048];
    char exe_list[1024]= {0};

    if (count == -1) {
        snprintf(exe_list, sizeof(exe_list), "ALL");
    } else {
        exe_list[0] = '\0';
        for (int i = 0; i < count && i < 20; i++) {
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s%s", exes[i], (i < count-1) ? "," : "");
            strncat(exe_list, tmp, sizeof(exe_list) - strlen(exe_list) - 1);
        }
    }

    snprintf(msg, sizeof(msg),
             "{\"command\":\"QUERY\",\"action\":%d,\"exes\":\"%s\"}\n",
             action, exe_list);

    send(notify_sock, msg, strlen(msg), 0);
}

static void
XnotifySendStatus(void) {
    if (notify_sock == -1)
        return;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"command\":\"STATUS\",\"enabled\":%s,\"security_mode\":%s}\n",
             xnotify_enabled ? "true" : "false",
             security_mode ? "true" : "false");
    send(notify_sock, msg, strlen(msg), 0);
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
XnotifyPoll(void) {
    if (command_sock == -1) {
        return;
    }

    struct pollfd pfd = { .fd = command_sock, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0) {
        if (guard_active)
            XnotifyIsGuardAlive();
        return;
    }

    static Bool permissions_changed = FALSE;
    char buf[512];

    while (poll(&pfd, 1, 0) > 0) {
        Bool  have_cred = FALSE;
        uid_t peer_uid  = (uid_t)-1;
        pid_t peer_pid  = -1;
        ssize_t n = XnotifyRecvCommand(command_sock, buf, sizeof(buf)-1,
                                       &have_cred, &peer_uid, &peer_pid);
        if (n <= 0) {
            XnotifyIsGuardAlive();
            break;
        }
        buf[n] = '\0';

        if (!is_trusted_sender(have_cred, peer_uid, peer_pid))
            continue;

        if (strstr(buf, "\"command\":\"XNOTIFY\"")) {
            pid_t pid = 0;

            char *p_pid = strstr(buf, "\"pid\":");
            if (p_pid) {
                sscanf(p_pid + 6, "%d", &pid);
            }
            XnotifyGuardHeartbeat(pid);
            continue;
        }

        if (strstr(buf, "\"command\":\"DISABLE\"")) {
            if (xnotify_enabled) {
                xnotify_enabled = FALSE;
                ErrorF("Xnotify: disabled at runtime (rules retained)\n");
            }
            XnotifySendStatus();
            continue;
        }
        if (strstr(buf, "\"command\":\"ENABLE\"")) {
            if (!xnotify_enabled) {
                xnotify_enabled = TRUE;
                XnotifyInvalidateAllClientCaches();
                ErrorF("Xnotify: re-enabled at runtime\n");
            }
            XnotifySendStatus();
            continue;
        }
        if (strstr(buf, "\"command\":\"STATUS\"")) {
            XnotifySendStatus();
            continue;
        }

        Bool has_arg = FALSE;

        if (strstr(buf, "\"command\":\"ALLOW\"")) {
            int action = 0;
            char exe[EXE_PATH_MAX] = {0};

            char *p_action = strstr(buf, "\"action\":");
            if (p_action) {
                sscanf(p_action + 9, "%d", &action);
            }

            char *p_exe = strstr(buf, "\"exe\":\"");
            if (p_exe) {
                p_exe += 7;
                char *end = strchr(p_exe, '"');
                if (end) { strncpy(exe, p_exe, end - p_exe); exe[end - p_exe] = '\0'; }
            }

            if (action > 0 && action <= XNOTIFY_MAX_ACTIONS && *exe) {
                const char *pipe = strchr(exe, '|');
                has_arg = pipe != NULL;
                if (has_arg) {
                    char exe_part[EXE_PATH_MAX];
                    strncpy(exe_part, exe, pipe - exe);
                    exe_part[pipe - exe] = '\0';
                    XnotifyAllowExe(action, exe_part, pipe + 1);
                } else
                    XnotifyAllowExe(action, exe, NULL);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"ALLOW_ACTION\"")) {
            int action = 0;
            char *p_action = strstr(buf, "\"action\":");
            if (p_action)
                sscanf(p_action + 9, "%d", &action);

            if (action > 0 && action <= XNOTIFY_MAX_ACTIONS) {
                XnotifyAllowAction(action);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"DENY\"")) {
            int action = 0;
            char exe[EXE_PATH_MAX] = {0};

            char *p_action = strstr(buf, "\"action\":");
            if (p_action) {
                sscanf(p_action + 9, "%d", &action);
            }

            char *p_exe = strstr(buf, "\"exe\":\"");
            if (p_exe) {
                p_exe += 7;
                char *end = strchr(p_exe, '"');
                if (end) { strncpy(exe, p_exe, end - p_exe); exe[end - p_exe] = '\0'; }
            }

            if (action > 0 && action <= XNOTIFY_MAX_ACTIONS && *exe) {
                XnotifyDenyExe(action, exe);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"ALLOW_ALL\"")) {
            char exe[EXE_PATH_MAX] = {0};

            char *p_exe = strstr(buf, "\"exe\":\"");
            if (p_exe) {
                p_exe += 7;
                char *end = strchr(p_exe, '"');
                if (end) { strncpy(exe, p_exe, end - p_exe); exe[end - p_exe] = '\0'; }
            }

            if (*exe) {
                const char *pipe = strchr(exe, '|');
                has_arg = pipe != NULL;
                if (has_arg) {
                    char exe_part[EXE_PATH_MAX];
                    strncpy(exe_part, exe, pipe - exe);
                    exe_part[pipe - exe] = '\0';
                    XnotifyAllowAll(exe_part, pipe + 1);
                } else
                    XnotifyAllowAll(exe, NULL);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"DENY_ALL\"")) {
            char exe[EXE_PATH_MAX] = {0};
            char *p_exe = strstr(buf, "\"exe\":\"");
            if (p_exe) {
                p_exe += 7;
                char *end = strchr(p_exe, '"');
                if (end) { strncpy(exe, p_exe, end - p_exe); exe[end - p_exe] = '\0'; }
            }
            if (*exe) {
                XnotifyDenyAll(exe);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"DENY_ACTION\"")) {
            int action = 0;
            char *p_action = strstr(buf, "\"action\":");
            if (p_action)
                sscanf(p_action + 9, "%d", &action);
            if (action > 0 && action <= XNOTIFY_MAX_ACTIONS) {
                XnotifyClear(action);
                permissions_changed = TRUE;
            }
        }
        else if (strstr(buf, "\"command\":\"QUERY_ACTION\"")) {
            int action = 0;
            char *p = strstr(buf, "\"action\":");
            if (p) {
                sscanf(p + 9, "%d", &action);
            }

            if (action > 0) {
                char exes[MAX_ALLOWED_EXES][EXE_PATH_MAX];
                int count = XnotifyQueryAction(action, exes, MAX_ALLOWED_EXES);
                XnotifySendQueryResponse(action, exes, count);
            }
        }
    }

    if (permissions_changed) {
        XnotifyInvalidateAllClientCaches();
        permissions_changed = FALSE;
    }

    XnotifyGuardHeartbeat(0);
}

void
Xnotify(ClientPtr client, const int action) {
    if (!xnotify_enabled)
        return;

    if (command_sock == -1)
        XnotifyInitCommand();

    if (!XnotifyIsGuardAlive())
        return;

    if (notify_sock == -1) {
        XnotifyInit();
        if (notify_sock == -1)
            return;
    }

    pid_t client_pid = GetClientPid(client);
    ClientExeInfo info = GetClientExeInfo(client);
    if (!info.exe) info.exe = strdup("?");

    char msg[4096];
    snprintf(msg, sizeof(msg),
             "{\"action\":%d,\"exe\":\"%s\",\"args\":\"%s\",\"pid\":%d}\n",
             action, info.exe, info.args ? info.args : "", client_pid);

    ssize_t len = strlen(msg);
    ssize_t sent = send(notify_sock, msg, len, 0);
    free(info.exe);
    free(info.args);

    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        close(notify_sock);
        notify_sock = -1;
    }
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

    XnotifyInitCommand();
}
