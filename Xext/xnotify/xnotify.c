/* SPDX-License-Identifier: MIT OR X11 OR GPL-3.0-or-later
 *
 * XNOTIFY extension - runtime permission gate and activity notifier.
 *
 * Lets an external, trusted "permission manager" (the guard) mediate what X11
 * clients are allowed to do - inject/grab input, take screenshots, manage other
 * windows, own the selection, etc. - on a per-executable basis, and be notified
 * when a client first attempts a sensitive action.
 *
 * Model: every sensitive action is a bit (see the XNOTIFY_* action bits in
 * xnotify.h). Permission state lives in three places, consulted in order by
 * XnotifyIsAllowed():
 *   1. a per-client cached bitmask (client devPrivate, via XnotifyPermMask),
 *   2. a global allow mask (xnotify_global_allow_mask),
 *   3. a rule table keyed by executable path/args (perm_hash_table +
 *      rule_list_head), populated from config files and guard commands.
 * On a miss the guard is notified (Xnotify) so it can prompt the user, and the
 * request is denied until a rule grants it.
 *
 * Two AF_UNIX/SOCK_DGRAM sockets carry the JSON protocol:
 *   - notify_sock  (server -> guard, connected): REPORT and notify messages.
 *   - command_sock (guard -> server, bound): heartbeats and commands. It is
 *     driven by the server poll loop via SetNotifyFd (XnotifyCommandReadable),
 *     so an idle desktop with no guard keeps zero timers armed.
 * Inbound messages are trusted only if they carry SO_PASSCRED peer credentials
 * matching the server uid (or root) AND come from an executable that itself
 * holds the XNOTIFY_GUARD permission (is_trusted_sender).
 *
 * The guard proves liveness with periodic heartbeats; if it goes silent for
 * GUARD_HEARTBEAT_TIMEOUT the server reverts to static config permissions
 * (XnotifyIsGuardAlive). Enforcement is bypassed entirely when the extension is
 * disabled at runtime (xnotify_enabled) or when neither a guard nor security
 * mode is active.
 */
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

/* Permission rules keyed by executable. Every rule lives in both structures:
 * perm_hash_table for O(1) exact-path lookup, and the rule_list_head/tail
 * singly-linked list for ordered pattern (wildcard) matching. */
XPermEntry *rule_list_head = NULL;               /**< head of the ordered rule list */
static XPermEntry *rule_list_tail = NULL;        /**< tail, for O(1) append */
XPermEntry *perm_hash_table[HASH_TABLE_SIZE] = {0}; /**< exact-path hash buckets */
uint32_t    xnotify_global_allow_mask = 0;       /**< actions allowed for every client */

/** One (exe, action) notification awaiting a guard decision, with its timestamp. */
typedef struct {
    char  exe[EXE_PATH_MAX];    /**< executable path that triggered the prompt */
    int   action;               /**< XNOTIFY_* action bit being requested */
    Time  last_time;            /**< when the guard was last notified about it */
} XPendingEntry;

/* Throttles repeat prompts: suppresses re-notifying the guard for the same
 * (exe, action) within XNOTIFY_PENDING_THROTTLE_MS. */
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

/**
 * @brief find the permission rule that applies to a client executable
 *
 * Tries an exact-path hash lookup first, then falls back to ordered wildcard
 * pattern matching over the rule list.
 *
 * @param info resolved executable path (and args) of the client
 * @return matching rule, or NULL if no rule applies
 */
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

/**
 * @brief return the exact-path rule for an executable, creating it if absent
 *
 * A newly created rule starts with an empty permission mask and is inserted
 * into both the hash table and the ordered rule list.
 *
 * @param exe  executable path (exact key)
 * @param args optional argument string stored on the rule, may be NULL
 * @return the existing or newly allocated rule, or NULL on allocation failure
 */
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

/**
 * @brief test a client (exe, args) against a rule pattern
 *
 * Rule strings may contain '*' wildcards; args are only compared when the rule
 * specifies an args pattern.
 *
 * @param exe       client executable path
 * @param args      client argument string, may be NULL
 * @param rule_exe  rule executable pattern
 * @param rule_args rule argument pattern, or NULL to match any args
 * @return TRUE if the client matches the rule
 */
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

/**
 * @brief reset all rules and reload permissions from the config files
 *
 * Clears the current rule table and re-reads the static allow/deny rules from
 * the configuration directory. Used at startup and when reverting to static
 * permissions after the guard disconnects.
 */
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

/**
 * @brief clear a single client's cached permission mask
 *
 * Forces the next XnotifyIsAllowed() for this client to re-evaluate against the
 * rule table (e.g. after its rules changed).
 *
 * @param client the client whose cached mask to drop
 */
void
XnotifyInvalidateClientCache(ClientPtr client) {
    if (client)
        *XnotifyPermMask(client) = 0;
}

/**
 * @brief clear every client's cached permission mask
 *
 * Used on a global permission change (config reload, guard death) so all clients
 * re-evaluate on their next action.
 */
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

/**
 * @brief grant an action to an executable (guard "allow" command)
 * @param action XNOTIFY_* action bit to grant
 * @param exe    executable path the rule applies to
 * @param args   optional argument pattern, may be NULL
 */
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

/**
 * @brief revoke an action from every rule matching an executable (guard "deny")
 * @param action XNOTIFY_* action bit to clear
 * @param exe    executable path to match (exact rule plus wildcard rules)
 */
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

/**
 * @brief grant every action to an executable
 * @param exe  executable path the rule applies to
 * @param args optional argument pattern, may be NULL
 */
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

/**
 * @brief revoke every action from an executable's exact-path rule
 * @param exe executable path whose rule to clear
 */
void
XnotifyDenyAll(const char *exe) {
    if (!exe || !*exe)
        return;

    XPermEntry *entry = find_exact_entry(exe);
    if (entry) {
        entry->perm_mask = 0;
    }
}

/**
 * @brief allow an action for every client, globally
 * @param action XNOTIFY_* action bit to add to the global allow mask
 */
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

/**
 * @brief list the executables granted a given action
 * @param action   XNOTIFY_* action bit to query
 * @param out_exes caller-provided array to fill with executable paths
 * @param max_exes capacity of @p out_exes
 * @return number of executables written, or -1 if the action is globally allowed
 */
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

/**
 * @brief revoke an action everywhere: global mask and all executable rules
 * @param action XNOTIFY_* action bit to clear
 */
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

/**
 * @brief resolve a pid to its executable path, with an LRU cache
 *
 * Reads /proc/<pid>/exe (falling back to /proc/<pid>/cmdline) on a cache miss
 * and stores the result; the returned pointer is owned by the cache and stays
 * valid until the entry is evicted.
 *
 * @param pid client process id
 * @return cached executable path, or NULL if it cannot be resolved
 */
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

/**
 * @brief resolve a client's executable path and argument string
 *
 * @param client the X11 client
 * @return owned copies in .exe/.args (caller must free); .exe is NULL if the
 *         executable cannot be resolved
 */
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

static int notify_sock = -1;   /**< server -> guard datagram socket (connected) */
static int command_sock = -1;  /**< guard -> server datagram socket (bound, poll-driven) */
/* Armed only while a permission manager (guard) is connected, so an idle
 * desktop with no guard keeps zero timers running (the command socket is
 * event-driven via SetNotifyFd, not polled). */
static OsTimerPtr guard_liveness_timer = NULL;

/* Guard liveness state. guard_active flips true on the first heartbeat and
 * back to false when heartbeats stop for GUARD_HEARTBEAT_TIMEOUT ms. */
static Bool guard_active = FALSE;          /**< is a guard currently connected? */
static Time last_guard_heartbeat = 0;      /**< timestamp of the last heartbeat */
#define GUARD_HEARTBEAT_TIMEOUT  5000      /**< ms of silence before the guard is dead */
static pid_t pid_guard = 0;                /**< pid of the connected guard */

static Bool xnotify_enabled = TRUE;        /**< runtime master switch (DISABLE command) */

static void XnotifyInitCommand(void);
static Bool XnotifyIsGuardAlive(void);
static void XnotifyCommandReadable(int fd, int ready, void *data);
static CARD32 XnotifyGuardLivenessTimer(OsTimerPtr timer, CARD32 now, void *arg);

/**
 * @brief record a guard heartbeat, marking the guard alive
 *
 * On the first heartbeat this marks the guard connected and arms the liveness
 * watchdog timer. No-op unless security mode is active.
 *
 * @param pid pid of the guard that sent the heartbeat
 */
static void
XnotifyGuardHeartbeat(pid_t pid) {
    if (!security_mode)
        return;

    last_guard_heartbeat = GetTimeInMillis();
    if (!guard_active) {
        guard_active = TRUE;
        pid_guard = pid;
        /* Start watching for the guard's death only now that one exists. */
        guard_liveness_timer = TimerSet(guard_liveness_timer, 0,
                                        GUARD_HEARTBEAT_TIMEOUT,
                                        XnotifyGuardLivenessTimer, NULL);
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

/**
 * @brief check whether the guard is still alive, reverting permissions if not
 *
 * If more than GUARD_HEARTBEAT_TIMEOUT ms have passed since the last heartbeat,
 * the guard is declared dead: dynamic permissions are cleared and static config
 * is reloaded. This is the lazy safety net called on every permission decision.
 *
 * @return TRUE if a guard is currently connected and alive
 */
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

/**
 * @brief decide whether an inbound command datagram may be trusted
 *
 * Requires SO_PASSCRED peer credentials whose uid matches the server (or root),
 * and that the sending process's executable itself holds XNOTIFY_GUARD. This is
 * what prevents an arbitrary local client from impersonating the guard.
 *
 * @param have_cred whether peer credentials were received
 * @param peer_uid  sender uid from SCM_CREDENTIALS
 * @param peer_pid  sender pid from SCM_CREDENTIALS
 * @return TRUE if the sender is an authorized guard
 */
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

/**
 * @brief receive one datagram plus the sender's peer credentials
 *
 * @param fd        the bound command socket
 * @param buf       output buffer for the message
 * @param buflen    size of @p buf
 * @param have_cred out: set TRUE if SCM_CREDENTIALS were present
 * @param peer_uid  out: sender uid (if have_cred)
 * @param peer_pid  out: sender pid (if have_cred)
 * @return bytes received, 0 on orderly shutdown, or <0 on error
 */
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

/**
 * @brief lazily open the outgoing (server -> guard) notify socket
 *
 * Connects a non-blocking datagram socket to the guard's notify socket path.
 * No-op if already open or if the guard is not listening.
 */
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

/**
 * @brief create and register the inbound (guard -> server) command socket
 *
 * Binds a non-blocking SO_PASSCRED datagram socket at the well-known command
 * path and registers it with the server poll loop (SetNotifyFd), so commands
 * are handled event-driven with no polling timer. No-op if already set up.
 */
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

    /* Drive the command socket from the server's poll loop instead of a
     * periodic timer: the server now only wakes when a datagram actually
     * arrives, so an idle desktop stays blocked in epoll (no wakeup storm). */
    if (!SetNotifyFd(command_sock, XnotifyCommandReadable, X_NOTIFY_READ, NULL))
        ErrorF("Xnotify: failed to register command socket with poll loop\n");
}

/**
 * @brief poll-loop callback: drain the command socket when it becomes readable
 *
 * Registered via SetNotifyFd; runs opportunistic cache cleanup and processes
 * all pending command datagrams.
 *
 * @param fd    the command socket (unused; module-global)
 * @param ready poll readiness mask (unused)
 * @param arg   user data (unused)
 */
static void
XnotifyCommandReadable(int fd, int ready, void *arg) {
    (void)fd; (void)ready; (void)arg;
    XnotifyCacheCleanup();
    XnotifyPoll();
}

/**
 * @brief watchdog that detects the guard dying while the system is idle
 *
 * Armed only while a guard is connected. Re-arms itself as long as the guard is
 * alive; once the guard is gone it returns 0 to stop, so an idle desktop with
 * no guard keeps zero timers running.
 *
 * @return the re-arm interval in ms, or 0 to stop the timer
 */
static CARD32
XnotifyGuardLivenessTimer(OsTimerPtr timer, CARD32 now, void *arg) {
    (void)timer; (void)now; (void)arg;
    if (XnotifyIsGuardAlive())
        return GUARD_HEARTBEAT_TIMEOUT;
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

static struct {
    char exe[EXE_PATH_MAX];
    int      action;
    Time     last_time;
} last_notify = {0};

#define REPORT_THROTTLE_SIZE 64
static struct {
    char exe[EXE_PATH_MAX];
    int  action;
    Time last_time;
} report_throttle[REPORT_THROTTLE_SIZE];
static int report_throttle_count = 0;

static Bool
XnotifyReportThrottled(const char *exe, int action)
{
    Time now = GetTimeInMillis();

    for (int i = 0; i < report_throttle_count; i++) {
        if (report_throttle[i].action != action)
            continue;
        if (strcmp(report_throttle[i].exe, exe) != 0)
            continue;
        if (now - report_throttle[i].last_time < XNOTIFY_REPORT_THROTTLE_MS)
            return TRUE;
        report_throttle[i].last_time = now;
        return FALSE;
    }

    int slot;
    if (report_throttle_count < REPORT_THROTTLE_SIZE) {
        slot = report_throttle_count++;
    } else {
        slot = 0;
        for (int i = 1; i < REPORT_THROTTLE_SIZE; i++) {
            if (report_throttle[i].last_time < report_throttle[slot].last_time)
                slot = i;
        }
    }
    strncpy(report_throttle[slot].exe, exe, EXE_PATH_MAX - 1);
    report_throttle[slot].exe[EXE_PATH_MAX - 1] = '\0';
    report_throttle[slot].action = action;
    report_throttle[slot].last_time = now;
    return FALSE;
}

static Bool
should_notify(const char *exe, const int action) {
    if (strcmp(exe, last_notify.exe) != 0 || action != last_notify.action)
        return TRUE;

    Time now = GetTimeInMillis();
    if (now - last_notify.last_time < XNOTIFY_THROTTLE_MS)
        return FALSE;

    return TRUE;
}

static void
update_last_notify(const char *exe, const int action) {
    strncpy(last_notify.exe, exe, EXE_PATH_MAX - 1);
    last_notify.exe[EXE_PATH_MAX - 1] = '\0';
    last_notify.action = action;
    last_notify.last_time = GetTimeInMillis();
}

static void
XnotifyGrantPermission(ClientPtr client, int action) {
    if (!client || action <= 0)
        return;

    uint32_t bit = (1U << (action - 1));
    *XnotifyPermMask(client) |= bit;
}

/**
 * @brief tell the guard that an already-permitted action just happened
 *
 * Telemetry only (not enforcement). Heavily throttled: a cheap per-(pid,action)
 * gate short-circuits bursts before any allocation or socket work, backed by the
 * (exe, action) report throttle. No-op if disabled or no guard is alive.
 *
 * @param client the client performing the action
 * @param action XNOTIFY_* action bit that was permitted
 */
void
XnotifyReport(ClientPtr client, const int action) {
    if (!xnotify_enabled)
        return;

    if (!XnotifyIsGuardAlive())
        return;

    if (notify_sock == -1) {
        XnotifyInit();
        if (notify_sock == -1)
            return;
    }

    pid_t pid = GetClientPid(client);

    static pid_t last_report_pid = 0;
    static int   last_report_action = 0;
    static Time  last_report_time = 0;
    Time now = GetTimeInMillis();
    if (pid == last_report_pid && action == last_report_action &&
        now - last_report_time < XNOTIFY_REPORT_THROTTLE_MS)
        return;
    last_report_pid = pid;
    last_report_action = action;
    last_report_time = now;

    const char *exe = XnotifyGetExe(pid);
    if (!exe)
        exe = "?";

    if (XnotifyReportThrottled(exe, action))
        return;

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "{\"command\":\"REPORT\",\"action\":%d,\"exe\":\"%s\",\"pid\":%d}\n",
             action, exe, pid);

    ssize_t sent = send(notify_sock, msg, strlen(msg), 0);

    if (sent == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        close(notify_sock);
        notify_sock = -1;
    }
}

/**
 * @brief central permission gate: may this client perform this action?
 *
 * Consulted throughout the server before sensitive operations. Fast-passes when
 * the extension is disabled or no guard/security is active. Otherwise checks, in
 * order: the per-client cached mask, the global allow mask, then a rule match on
 * the client's executable. On a rule grant the client mask is updated so future
 * checks are O(1); on a miss the guard is notified (throttled) and the action is
 * denied until a rule allows it.
 *
 * @param client the requesting X11 client
 * @param action XNOTIFY_* action bit being attempted
 * @return TRUE if allowed, FALSE if denied
 */
Bool
XnotifyIsAllowed(ClientPtr client, const int action) {
    if (!xnotify_enabled)
        return TRUE;

    if (!XnotifyIsGuardAlive() && !security_mode)
        return TRUE;

    uint32_t bit = (1U << (action - 1));

    if (*XnotifyPermMask(client) & bit) {
        XnotifyReport(client, action);
        return TRUE;
    }

    if (xnotify_global_allow_mask & bit) {
        XnotifyGrantPermission(client, action);
        XnotifyReport(client, action);
        return TRUE;
    }

    ClientExeInfo info = GetClientExeInfo(client);
    if (!info.exe)
        return FALSE;

    XPermEntry *entry = find_matching_entry(&info);
    Bool allowed = FALSE;
    if (entry && (entry->perm_mask & bit)) {
        XnotifyGrantPermission(client, action);
        XnotifyRemovePending(info.exe, action);
        allowed = TRUE;
    }

    if (XnotifyHasRecentPending(info.exe, action)) {
        free(info.exe);
        free(info.args);
        return allowed;
    }

    if (allowed) {
        XnotifyReport(client, action);
        free(info.exe);
        free(info.args);
        return TRUE;
    }

    if (should_notify(info.exe, action)) {
        Xnotify(client, action);
        update_last_notify(info.exe, action);
        XnotifyAddPending(info.exe, action);
    }

    free(info.exe);
    free(info.args);
    return FALSE;
}

/**
 * @brief drain and process all pending command datagrams from the guard
 *
 * Reads each queued message, verifies the sender (is_trusted_sender), and
 * dispatches heartbeats and commands (ENABLE/DISABLE/allow/deny/query/status).
 * Invoked by XnotifyCommandReadable when the poll loop reports the socket
 * readable.
 */
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

/**
 * @brief send a full notification to the guard so it can prompt the user
 *
 * Emitted when a client attempts an action no rule yet covers. Includes the
 * client's executable, args, and pid. Lazily opens both sockets; no-op if
 * disabled or no guard is alive.
 *
 * @param client the client whose action triggered the prompt
 * @param action XNOTIFY_* action bit being requested
 */
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

/**
 * @brief top-level request handler for the XNOTIFY extension
 * @param client the requesting client
 * @return an X11 request status (Success or BadRequest for unknown minor ops)
 */
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

/**
 * @brief register the XNOTIFY extension and bring up the command socket
 *
 * Registers the per-client permission-mask private and the protocol extension,
 * then initializes the (event-driven) command socket. Called once at server
 * start via the extension init table.
 */
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
