// ============================================================
//  Metalog - Advanced Per-Plugin Logging & Debugging Module
//  for AMX Mod X
//  Version: 2.0.0
//
//  Features
//   - Per-plugin state (detected automatically from the caller)
//   - metalog_dial()  : prints info ONLY about the calling plugin
//   - metalog_off(1/0): mute / unmute Metalog for the calling plugin
//   - Per-plugin level mask, tag prefix and optional own log file
//   - History ring buffer (last N log lines per plugin)
//   - Per-level counters, once / every / assert / array dump
//   - High-resolution timers (per plugin) + metalog_perf()
//   - Log file rotation (size limit -> .old)
//   - Extra destination: AMXX log (MD_AMXX)
//
//  NOTE: optional but recommended - in moduleconfig.h enable:
//     #define FN_AMXX_PLUGINSUNLOADED OnPluginsUnloaded
//  (state is also reset lazily, so it works without it)
// ============================================================

#include "amxxmodule.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
	#include <windows.h>
	#include <direct.h>
	#define ML_MKDIR(p) _mkdir(p)
#else
	#include <sys/time.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#define ML_MKDIR(p) mkdir(p, 0755)
#endif

#define METALOG_VERSION   "2.0.0"
#define ML_OUT               MF_PrintSrvConsole

// ============================================================
//  Enums (must match metalog.inc)
// ============================================================

enum MetalogLevel
{
	ML_NONE   = 0,
	ML_ERROR  = 1,
	ML_WARN   = 2,
	ML_INFO   = 4,
	ML_DEBUG  = 8,
	ML_TRACE  = 16,
	ML_PERF   = 32,
	ML_ALL    = 63
};

enum MetalogDest
{
	MD_NONE    = 0,
	MD_SERVER  = 1,
	MD_CLIENTS = 2,		// reserved - handled by stocks in metalog.inc
	MD_FILE    = 4,
	MD_CHAT    = 8,		// reserved - handled by stocks in metalog.inc
	MD_AMXX    = 16		// AMXX log (MF_Log)
};

// ============================================================
//  Limits
// ============================================================

#define MAX_PLUGINS     128
#define MAX_TIMERS      32
#define MAX_ONCE        32
#define MAX_EVERY       16
#define HISTORY_SIZE    32
#define HISTORY_LEN     192
#define MSG_LEN         1024
#define LOG_DIR         "addons/amxmodx/logs/metalog"

static const char* LEVEL_NAMES[6] = { "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE", "PERF " };

// ============================================================
//  State
// ============================================================

struct GlobalState
{
	bool    enabled;
	int     level;
	int     dest;
	char    file[256];
	FILE*   fp;
	long    maxSize;        // bytes, 0 = no rotation
	bool    errShown;
	time_t  started;
	unsigned int total;
};

static GlobalState g_G =
{
	true,
	ML_ERROR | ML_WARN | ML_INFO | ML_DEBUG,
	MD_SERVER | MD_FILE,
	"addons/amxmodx/logs/metalog.log",
	nullptr,
	5 * 1024 * 1024,
	false,
	0,
	0
};

struct HistEntry
{
	time_t t;
	int    level;
	char   text[HISTORY_LEN];
};

struct EveryEntry
{
	unsigned int hash;
	unsigned int counter;
};

struct PluginState
{
	bool        used;
	const AMX*  amx;
	int         id;
	char        name[64];

	bool        off;            // metalog_off
	int         level;          // -1 = follow global
	char        tag[32];
	bool        ownFile;
	FILE*       fp;
	char        filePath[256];
	bool        errShown;

	unsigned int counts[6];
	unsigned int total;
	time_t      first;
	time_t      last;

	HistEntry   hist[HISTORY_SIZE];
	int         histHead;
	int         histCount;

	bool        timerActive[MAX_TIMERS];
	double      timerStart[MAX_TIMERS];
	double      perfThresholdMs;

	char        onceKeys[MAX_ONCE][48];
	int         onceCount;

	EveryEntry  every[MAX_EVERY];
	int         everyCount;
};

static PluginState g_Plugins[MAX_PLUGINS];
static PluginState g_Unknown;

// ============================================================
//  Helpers
// ============================================================

static double GetHighResTime()
{
#ifdef _WIN32
	static LARGE_INTEGER freq = {0};
	if (freq.QuadPart == 0)
		QueryPerformanceFrequency(&freq);

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
#else
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

static void FormatTime(time_t t, char* buf, size_t max, bool withDate)
{
	struct tm* lt = localtime(&t);
	if (!lt) { snprintf(buf, max, "--:--:--"); return; }

	if (withDate)
		snprintf(buf, max, "%04d-%02d-%02d %02d:%02d:%02d",
			lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
	else
		snprintf(buf, max, "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
}

static int LevelIndex(int level)
{
	if (level & ML_ERROR) return 0;
	if (level & ML_WARN)  return 1;
	if (level & ML_PERF)  return 5;
	if (level & ML_DEBUG) return 3;
	if (level & ML_TRACE) return 4;
	return 2;
}

static void LevelMaskToString(int mask, char* buf, size_t max)
{
	buf[0] = '\0';
	if (mask & ML_ERROR) strncat(buf, "ERROR ", max - strlen(buf) - 1);
	if (mask & ML_WARN)  strncat(buf, "WARN ",  max - strlen(buf) - 1);
	if (mask & ML_INFO)  strncat(buf, "INFO ",  max - strlen(buf) - 1);
	if (mask & ML_DEBUG) strncat(buf, "DEBUG ", max - strlen(buf) - 1);
	if (mask & ML_TRACE) strncat(buf, "TRACE ", max - strlen(buf) - 1);
	if (mask & ML_PERF)  strncat(buf, "PERF ",  max - strlen(buf) - 1);
	if (!buf[0])         strncat(buf, "(none)", max - 1);
}

static void DestMaskToString(int mask, char* buf, size_t max)
{
	buf[0] = '\0';
	if (mask & MD_SERVER) strncat(buf, "SERVER ", max - strlen(buf) - 1);
	if (mask & MD_FILE)   strncat(buf, "FILE ",   max - strlen(buf) - 1);
	if (mask & MD_AMXX)   strncat(buf, "AMXX ",   max - strlen(buf) - 1);
	if (!buf[0])          strncat(buf, "(none)",  max - 1);
}

static unsigned int HashString(const char* s)
{
	unsigned int h = 2166136261u;		// FNV-1a
	while (*s)
	{
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

static void CloseFile(FILE*& fp)
{
	if (fp)
	{
		fclose(fp);
		fp = nullptr;
	}
}

// ============================================================
//  Plugin state management
// ============================================================

static void ResetPlugin(PluginState* p, const AMX* amx, int id)
{
	CloseFile(p->fp);
	memset(p, 0, sizeof(PluginState));

	p->used  = true;
	p->amx   = amx;
	p->id    = id;
	p->level = -1;

	const char* nm = (id >= 0) ? MF_GetScriptName(id) : nullptr;
	if (!nm || !nm[0])
		nm = "unknown.amxx";

	// keep only the file name, drop any directory part
	const char* slash = strrchr(nm, '/');
	const char* bslash = strrchr(nm, '\\');
	if (bslash > slash) slash = bslash;
	if (slash) nm = slash + 1;

	strncpy(p->name, nm, sizeof(p->name) - 1);

	// safe file name for the optional per-plugin log
	char safe[64];
	strncpy(safe, p->name, sizeof(safe) - 1);
	safe[sizeof(safe) - 1] = '\0';
	char* dot = strrchr(safe, '.');
	if (dot) *dot = '\0';
	for (char* c = safe; *c; ++c)
	{
		if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
		      (*c >= '0' && *c <= '9') || *c == '_' || *c == '-'))
			*c = '_';
	}
	snprintf(p->filePath, sizeof(p->filePath), LOG_DIR "/%s.log", safe);
}

static PluginState* GetPlugin(AMX* amx)
{
	int id = MF_FindScriptByAmx(amx);
	PluginState* p = (id >= 0 && id < MAX_PLUGINS) ? &g_Plugins[id] : &g_Unknown;

	// a different amx in the same slot = plugin was reloaded / changed map
	if (!p->used || p->amx != amx)
		ResetPlugin(p, amx, id);

	return p;
}

static void ResetAllPlugins()
{
	for (int i = 0; i < MAX_PLUGINS; i++)
	{
		CloseFile(g_Plugins[i].fp);
		memset(&g_Plugins[i], 0, sizeof(PluginState));
	}
	CloseFile(g_Unknown.fp);
	memset(&g_Unknown, 0, sizeof(PluginState));
}

// ============================================================
//  File output (with rotation)
// ============================================================

static void WriteLine(FILE*& fp, const char* relPath, bool& errShown, const char* line)
{
	char fullPath[256];
	MF_BuildPathnameR(fullPath, sizeof(fullPath), "%s", relPath);

	if (!fp)
	{
		char dir[256];
		MF_BuildPathnameR(dir, sizeof(dir), "%s", LOG_DIR);
		ML_MKDIR(dir);		// harmless if it already exists

		fp = fopen(fullPath, "a");
		if (!fp)
		{
			if (!errShown)
			{
				ML_OUT("[Metalog] ERROR: Could not open log file: %s\n", fullPath);
				errShown = true;
			}
			return;
		}
		errShown = false;
	}

	fputs(line, fp);
	fputc('\n', fp);
	fflush(fp);

	if (g_G.maxSize > 0 && ftell(fp) > g_G.maxSize)
	{
		CloseFile(fp);

		char oldPath[300];
		snprintf(oldPath, sizeof(oldPath), "%s.old", fullPath);
		remove(oldPath);
		rename(fullPath, oldPath);
		// file is reopened lazily on the next write
	}
}

// ============================================================
//  Core logger
// ============================================================

static void HistoryPush(PluginState* p, int level, const char* msg)
{
	HistEntry& e = p->hist[p->histHead];
	e.t     = time(nullptr);
	e.level = level;
	strncpy(e.text, msg, HISTORY_LEN - 1);
	e.text[HISTORY_LEN - 1] = '\0';

	p->histHead = (p->histHead + 1) % HISTORY_SIZE;
	if (p->histCount < HISTORY_SIZE)
		p->histCount++;
}

static void DoLog(PluginState* p, int level, int dest, const char* message)
{
	if (!g_G.enabled || p->off)
		return;

	int effective = (p->level >= 0) ? p->level : g_G.level;
	if (!(level & effective))
		return;

	char timeStr[16];
	FormatTime(time(nullptr), timeStr, sizeof(timeStr), false);

	int idx = LevelIndex(level);

	char full[MSG_LEN + 160];
	if (p->tag[0])
		snprintf(full, sizeof(full), "[%s] [%s] [%s] [%s] %s", timeStr, LEVEL_NAMES[idx], p->name, p->tag, message);
	else
		snprintf(full, sizeof(full), "[%s] [%s] [%s] %s", timeStr, LEVEL_NAMES[idx], p->name, message);

	// bookkeeping
	p->counts[idx]++;
	p->total++;
	g_G.total++;
	if (!p->first) p->first = time(nullptr);
	p->last = time(nullptr);
	HistoryPush(p, level, message);

	// destinations
	if (dest & MD_SERVER)
		ML_OUT("%s\n", full);

	if (dest & MD_FILE)
	{
		if (p->ownFile)
			WriteLine(p->fp, p->filePath, p->errShown, full);
		else
			WriteLine(g_G.fp, g_G.file, g_G.errShown, full);
	}

	if (dest & MD_AMXX)
		MF_Log("%s", full);
}

static inline int ParamCount(cell* params)
{
	return (int)(params[0] / sizeof(cell));
}

// ============================================================
//  Dial / diagnostics
// ============================================================

static void PrintLine(char c, int n)
{
	char buf[96];
	if (n > 90) n = 90;
	for (int i = 0; i < n; i++) buf[i] = c;
	buf[n] = '\0';
	ML_OUT("%s\n", buf);
}

static void PrintDial(PluginState* p, int history)
{
	char lv[96], ds[96], t1[32], t2[32];

	ML_OUT("\n");
	PrintLine('=', 64);
	ML_OUT("  METALOG DIAL  ::  %s  (plugin id %d)\n", p->name, p->id);
	PrintLine('=', 64);

	ML_OUT("  Debug state      : %s  (metalog_off %d)\n", p->off ? "OFF" : "ON", p->off ? 1 : 0);
	ML_OUT("  Global enabled   : %s\n", g_G.enabled ? "YES" : "NO");

	int effective = (p->level >= 0) ? p->level : g_G.level;
	LevelMaskToString(effective, lv, sizeof(lv));
	ML_OUT("  Level mask       : 0x%02X  (%s) %s\n", effective, lv, (p->level >= 0) ? "[plugin override]" : "[global]");

	DestMaskToString(g_G.dest, ds, sizeof(ds));
	ML_OUT("  Destinations     : %s\n", ds);
	ML_OUT("  Tag              : %s\n", p->tag[0] ? p->tag : "(none)");

	if (p->ownFile)
		ML_OUT("  Log file         : %s  [own file, %s]\n", p->filePath, p->fp ? "OPEN" : "CLOSED");
	else
		ML_OUT("  Log file         : %s  [global, %s]\n", g_G.file, g_G.fp ? "OPEN" : "CLOSED");

	ML_OUT("\n  [STATISTICS]\n");
	ML_OUT("  Total logged     : %u\n", p->total);
	ML_OUT("  By level         : ERROR %u | WARN %u | INFO %u | DEBUG %u | TRACE %u | PERF %u\n",
		p->counts[0], p->counts[1], p->counts[2], p->counts[3], p->counts[4], p->counts[5]);

	if (p->first)
	{
		FormatTime(p->first, t1, sizeof(t1), true);
		FormatTime(p->last,  t2, sizeof(t2), true);
		ML_OUT("  First log        : %s\n", t1);
		ML_OUT("  Last log         : %s\n", t2);
	}
	else
	{
		ML_OUT("  First/Last log   : (nothing logged yet)\n");
	}

	ML_OUT("  Once-keys used   : %d / %d\n", p->onceCount, MAX_ONCE);
	ML_OUT("  Every-counters   : %d / %d\n", p->everyCount, MAX_EVERY);

	ML_OUT("\n  [TIMERS]\n");
	bool any = false;
	double now = GetHighResTime();
	for (int i = 0; i < MAX_TIMERS; i++)
	{
		if (p->timerActive[i])
		{
			ML_OUT("  Timer[%2d]        : RUNNING  (%.6f sec)\n", i, now - p->timerStart[i]);
			any = true;
		}
	}
	if (!any)
		ML_OUT("  No active timers\n");
	if (p->perfThresholdMs > 0.0)
		ML_OUT("  Perf threshold   : %.3f ms\n", p->perfThresholdMs);

	if (history > 0 && p->histCount > 0)
	{
		if (history > p->histCount) history = p->histCount;

		ML_OUT("\n  [LAST %d ENTRIES]\n", history);
		for (int i = history; i > 0; i--)
		{
			int idx = (p->histHead - i + HISTORY_SIZE * 2) % HISTORY_SIZE;
			HistEntry& e = p->hist[idx];
			FormatTime(e.t, t1, sizeof(t1), false);
			ML_OUT("  [%s] [%s] %s\n", t1, LEVEL_NAMES[LevelIndex(e.level)], e.text);
		}
	}

	PrintLine('=', 64);
	ML_OUT("\n");
}

// ============================================================
//  Natives - logging
// ============================================================

// metalog(level, const fmt[], any:...)
static cell AMX_NATIVE_CALL n_metalog(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;
	char* msg = MF_FormatAmxString(amx, params, 2, &len);
	DoLog(p, params[1], g_G.dest, msg);
	return 1;
}

// metalog_ex(level, dest, const fmt[], any:...)
static cell AMX_NATIVE_CALL n_metalog_ex(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;
	char* msg = MF_FormatAmxString(amx, params, 3, &len);
	DoLog(p, params[1], params[2], msg);
	return 1;
}

// metalog_tag(level, const tag[], const fmt[], any:...)
static cell AMX_NATIVE_CALL n_metalog_tag(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;

	char tag[64];
	strncpy(tag, MF_GetAmxString(amx, params[2], 0, &len), sizeof(tag) - 1);
	tag[sizeof(tag) - 1] = '\0';

	char* msg = MF_FormatAmxString(amx, params, 3, &len);

	char combined[MSG_LEN];
	snprintf(combined, sizeof(combined), "[%s] %s", tag, msg);

	DoLog(p, params[1], g_G.dest, combined);
	return 1;
}

// metalog_once(level, const key[], const fmt[], any:...)
static cell AMX_NATIVE_CALL n_metalog_once(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;

	char key[48];
	strncpy(key, MF_GetAmxString(amx, params[2], 0, &len), sizeof(key) - 1);
	key[sizeof(key) - 1] = '\0';

	for (int i = 0; i < p->onceCount; i++)
	{
		if (strcmp(p->onceKeys[i], key) == 0)
			return 0;		// already printed
	}

	// if the table is full we still log (fail open - never lose a message)
	if (p->onceCount < MAX_ONCE)
	{
		strcpy(p->onceKeys[p->onceCount], key);
		p->onceCount++;
	}

	char* msg = MF_FormatAmxString(amx, params, 3, &len);
	DoLog(p, params[1], g_G.dest, msg);
	return 1;
}

// metalog_every(level, count, const fmt[], any:...)
// Logs on the 1st call, then on every Nth call of that specific message.
static cell AMX_NATIVE_CALL n_metalog_every(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;

	unsigned int count = (params[2] > 1) ? (unsigned int)params[2] : 1;
	unsigned int hash  = HashString(MF_GetAmxString(amx, params[3], 0, &len)) ^ (unsigned int)params[1];

	EveryEntry* e = nullptr;
	for (int i = 0; i < p->everyCount; i++)
	{
		if (p->every[i].hash == hash) { e = &p->every[i]; break; }
	}

	if (!e)
	{
		int slot = (p->everyCount < MAX_EVERY) ? p->everyCount++ : (int)(hash % MAX_EVERY);
		e = &p->every[slot];
		e->hash = hash;
		e->counter = 0;
	}

	bool fire = (e->counter % count) == 0;
	e->counter++;

	if (!fire)
		return 0;

	char* msg = MF_FormatAmxString(amx, params, 3, &len);
	DoLog(p, params[1], g_G.dest, msg);
	return 1;
}

// metalog_assert(bool:condition, const fmt[], any:...)  -> returns condition
static cell AMX_NATIVE_CALL n_metalog_assert(AMX *amx, cell *params)
{
	if (params[1])
		return 1;

	PluginState* p = GetPlugin(amx);
	int len;
	char* msg = MF_FormatAmxString(amx, params, 2, &len);

	char combined[MSG_LEN];
	snprintf(combined, sizeof(combined), "ASSERT FAILED: %s", msg);
	DoLog(p, ML_ERROR, g_G.dest, combined);
	return 0;
}

// metalog_dump(level, const label[], const data[], size)
static cell AMX_NATIVE_CALL n_metalog_dump(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;

	char label[64];
	strncpy(label, MF_GetAmxString(amx, params[2], 0, &len), sizeof(label) - 1);
	label[sizeof(label) - 1] = '\0';

	cell* data = MF_GetAmxAddr(amx, params[3]);
	int size = params[4];
	if (!data || size <= 0)
		return 0;

	const int SHOW = 32;
	int n = (size < SHOW) ? size : SHOW;

	char buf[MSG_LEN];
	int pos = snprintf(buf, sizeof(buf), "%s[%d] = {", label, size);

	for (int i = 0; i < n && pos < (int)sizeof(buf) - 24; i++)
		pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%d", i ? ", " : "", (int)data[i]);

	if (size > n)
		pos += snprintf(buf + pos, sizeof(buf) - pos, ", ... +%d more", size - n);

	snprintf(buf + pos, sizeof(buf) - pos, "}");

	DoLog(p, params[1], g_G.dest, buf);
	return 1;
}

// ============================================================
//  Natives - per-plugin control
// ============================================================

// metalog_off(off)  : 1 = mute Metalog for this plugin, 0 = enable
static cell AMX_NATIVE_CALL n_metalog_off(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	bool newState = params[1] != 0;

	if (newState != p->off)
	{
		p->off = newState;
		ML_OUT("[Metalog] %s : debug %s\n", p->name, p->off ? "OFF" : "ON");
	}
	return 1;
}

static cell AMX_NATIVE_CALL n_metalog_is_off(AMX *amx, cell *params)
{
	return GetPlugin(amx)->off ? 1 : 0;
}

// metalog_plugin_level(level) : -1 = follow global
static cell AMX_NATIVE_CALL n_metalog_plugin_level(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	p->level = (params[1] < 0) ? -1 : (params[1] & ML_ALL);
	return 1;
}

// metalog_plugin_tag(const tag[])
static cell AMX_NATIVE_CALL n_metalog_plugin_tag(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int len;
	strncpy(p->tag, MF_GetAmxString(amx, params[1], 0, &len), sizeof(p->tag) - 1);
	p->tag[sizeof(p->tag) - 1] = '\0';
	return 1;
}

// metalog_plugin_file(bool:enable) : own log file logs/metalog/<plugin>.log
static cell AMX_NATIVE_CALL n_metalog_plugin_file(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	p->ownFile = params[1] != 0;
	if (!p->ownFile)
		CloseFile(p->fp);
	return 1;
}

// metalog_dial(history = 8, bool:force = false)
static cell AMX_NATIVE_CALL n_metalog_dial(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int  history = (ParamCount(params) >= 1) ? params[1] : 8;
	bool force   = (ParamCount(params) >= 2) ? (params[2] != 0) : false;

	// Dial only answers when debug is ON for this plugin (unless forced)
	if (p->off && !force)
		return 0;

	PrintDial(p, history);
	return 1;
}

// metalog_get_count(level = ML_ALL)
static cell AMX_NATIVE_CALL n_metalog_get_count(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int mask = (ParamCount(params) >= 1) ? params[1] : ML_ALL;

	unsigned int sum = 0;
	for (int i = 0; i < 6; i++)
	{
		static const int bits[6] = { ML_ERROR, ML_WARN, ML_INFO, ML_DEBUG, ML_TRACE, ML_PERF };
		if (mask & bits[i])
			sum += p->counts[i];
	}
	return (cell)sum;
}

// metalog_clear() : wipe stats + history of the calling plugin
static cell AMX_NATIVE_CALL n_metalog_clear(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	memset(p->counts, 0, sizeof(p->counts));
	p->total = 0;
	p->first = p->last = 0;
	p->histHead = p->histCount = 0;
	p->onceCount = 0;
	p->everyCount = 0;
	return 1;
}

// ============================================================
//  Natives - global configuration
// ============================================================

static cell AMX_NATIVE_CALL n_metalog_set_level(AMX *amx, cell *params) { g_G.level = params[1] & ML_ALL; return 1; }
static cell AMX_NATIVE_CALL n_metalog_get_level(AMX *amx, cell *params) { return g_G.level; }
static cell AMX_NATIVE_CALL n_metalog_set_dest(AMX *amx, cell *params)  { g_G.dest = params[1]; return 1; }
static cell AMX_NATIVE_CALL n_metalog_get_dest(AMX *amx, cell *params)  { return g_G.dest; }
static cell AMX_NATIVE_CALL n_metalog_enable(AMX *amx, cell *params)    { g_G.enabled = params[1] != 0; return 1; }
static cell AMX_NATIVE_CALL n_metalog_is_enabled(AMX *amx, cell *params){ return g_G.enabled ? 1 : 0; }

static cell AMX_NATIVE_CALL n_metalog_set_file(AMX *amx, cell *params)
{
	int len;
	char *path = MF_GetAmxString(amx, params[1], 0, &len);

	strncpy(g_G.file, path, sizeof(g_G.file) - 1);
	g_G.file[sizeof(g_G.file) - 1] = '\0';

	CloseFile(g_G.fp);
	g_G.errShown = false;

	char fullPath[256];
	MF_BuildPathnameR(fullPath, sizeof(fullPath), "%s", g_G.file);
	ML_OUT("[Metalog] Log file path set to: %s\n", fullPath);
	return 1;
}

// metalog_set_maxsize(kb) : 0 disables rotation
static cell AMX_NATIVE_CALL n_metalog_set_maxsize(AMX *amx, cell *params)
{
	g_G.maxSize = (params[1] > 0) ? (long)params[1] * 1024L : 0;
	return 1;
}

static cell AMX_NATIVE_CALL n_metalog_flush(AMX *amx, cell *params)
{
	if (g_G.fp) fflush(g_G.fp);
	for (int i = 0; i < MAX_PLUGINS; i++)
		if (g_Plugins[i].fp) fflush(g_Plugins[i].fp);
	return 1;
}

// metalog_diag() / metalog_dial_all() : global report with all plugins
static cell AMX_NATIVE_CALL n_metalog_diag(AMX *amx, cell *params)
{
	char lv[96], ds[96], tb[40];

	ML_OUT("\n");
	PrintLine('=', 78);
	ML_OUT("                 METALOG v%s - GLOBAL DIAGNOSTIC REPORT\n", METALOG_VERSION);
	PrintLine('=', 78);

	ML_OUT("\n  [MODULE STATUS]\n");
	LevelMaskToString(g_G.level, lv, sizeof(lv));
	DestMaskToString(g_G.dest, ds, sizeof(ds));
	ML_OUT("  Enabled          : %s\n", g_G.enabled ? "YES" : "NO");
	ML_OUT("  Log Level        : 0x%02X  (%s)\n", g_G.level, lv);
	ML_OUT("  Destinations     : %s\n", ds);
	ML_OUT("  Log File         : %s  [%s]\n", g_G.file, g_G.fp ? "OPEN" : "CLOSED");
	ML_OUT("  Rotation limit   : %ld KB\n", g_G.maxSize / 1024);
	ML_OUT("  Total messages   : %u\n", g_G.total);

	ML_OUT("\n  [SERVER INFO]\n");
	FormatTime(time(nullptr), tb, sizeof(tb), true);
	ML_OUT("  Current Time     : %s\n", tb);
	double t1 = GetHighResTime();
	double t2 = GetHighResTime();
	ML_OUT("  High-Res Timer   : Working (delta %.6f sec)\n", t2 - t1);

	ML_OUT("\n  [PLUGINS USING METALOG]\n");
	ML_OUT("  %-24s %-5s %-7s %-7s %-7s %-7s\n", "Plugin", "State", "Total", "Errors", "Warns", "Timers");
	PrintLine('-', 78);

	int shown = 0;
	for (int i = 0; i < MAX_PLUGINS; i++)
	{
		PluginState* p = &g_Plugins[i];
		if (!p->used) continue;

		int timers = 0;
		for (int t = 0; t < MAX_TIMERS; t++)
			if (p->timerActive[t]) timers++;

		ML_OUT("  %-24s %-5s %-7u %-7u %-7u %-7d\n",
			p->name, p->off ? "OFF" : "ON", p->total, p->counts[0], p->counts[1], timers);
		shown++;
	}
	if (!shown)
		ML_OUT("  (no plugin has called Metalog yet)\n");

	ML_OUT("\n  Tip: call metalog_dial() inside a plugin to see only that plugin.\n");
	ML_OUT("  Tip: metalog_off(1) / metalog_off(0) mutes / unmutes a single plugin.\n");

	PrintLine('=', 78);
	ML_OUT("\n");
	return 1;
}

// ============================================================
//  Natives - high resolution timers (per plugin)
// ============================================================

static cell AMX_NATIVE_CALL n_metalog_tick(AMX *amx, cell *params)
{
	return amx_ftoc((float)GetHighResTime());
}

static cell AMX_NATIVE_CALL n_metalog_tick_start(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int id = params[1];
	if (id < 0 || id >= MAX_TIMERS) return 0;

	p->timerActive[id] = true;
	p->timerStart[id]  = GetHighResTime();
	return 1;
}

static cell AMX_NATIVE_CALL n_metalog_tick_stop(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int id = params[1];
	if (id < 0 || id >= MAX_TIMERS || !p->timerActive[id])
		return amx_ftoc(0.0f);

	double elapsed = GetHighResTime() - p->timerStart[id];
	p->timerActive[id] = false;
	return amx_ftoc((float)elapsed);
}

static cell AMX_NATIVE_CALL n_metalog_tick_elapsed(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int id = params[1];
	if (id < 0 || id >= MAX_TIMERS || !p->timerActive[id])
		return amx_ftoc(0.0f);

	return amx_ftoc((float)(GetHighResTime() - p->timerStart[id]));
}

static cell AMX_NATIVE_CALL n_metalog_tick_reset(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int id = params[1];
	if (id < 0 || id >= MAX_TIMERS) return 0;

	p->timerActive[id] = false;
	p->timerStart[id]  = 0.0;
	return 1;
}

// Float:metalog_perf(id, const label[] = "")
// Stops the timer and logs a PERF line (only if >= threshold, when one is set).
static cell AMX_NATIVE_CALL n_metalog_perf(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	int id = params[1];
	if (id < 0 || id >= MAX_TIMERS || !p->timerActive[id])
		return amx_ftoc(0.0f);

	double elapsed = GetHighResTime() - p->timerStart[id];
	p->timerActive[id] = false;

	double ms = elapsed * 1000.0;
	if (p->perfThresholdMs <= 0.0 || ms >= p->perfThresholdMs)
	{
		int len;
		const char* label = (ParamCount(params) >= 2) ? MF_GetAmxString(amx, params[2], 0, &len) : "";
		char buf[MSG_LEN];
		snprintf(buf, sizeof(buf), "%s%stook %.3f ms", label, label[0] ? " " : "", ms);
		DoLog(p, ML_PERF, g_G.dest, buf);
	}
	return amx_ftoc((float)elapsed);
}

// metalog_perf_threshold(Float:ms)
static cell AMX_NATIVE_CALL n_metalog_perf_threshold(AMX *amx, cell *params)
{
	PluginState* p = GetPlugin(amx);
	float ms = amx_ctof(params[1]);
	p->perfThresholdMs = (ms > 0.0f) ? (double)ms : 0.0;
	return 1;
}

// ============================================================
//  Native table
// ============================================================

AMX_NATIVE_INFO metalog_natives[] =
{
	// logging
	{ "metalog",                n_metalog },
	{ "metalog_ex",             n_metalog_ex },
	{ "metalog_tag",            n_metalog_tag },
	{ "metalog_once",           n_metalog_once },
	{ "metalog_every",          n_metalog_every },
	{ "metalog_assert",         n_metalog_assert },
	{ "metalog_dump",           n_metalog_dump },

	// per plugin
	{ "metalog_off",            n_metalog_off },
	{ "metalog_is_off",         n_metalog_is_off },
	{ "metalog_plugin_level",   n_metalog_plugin_level },
	{ "metalog_plugin_tag",     n_metalog_plugin_tag },
	{ "metalog_plugin_file",    n_metalog_plugin_file },
	{ "metalog_dial",           n_metalog_dial },
	{ "metalog_get_count",      n_metalog_get_count },
	{ "metalog_clear",          n_metalog_clear },

	// global config
	{ "metalog_set_level",      n_metalog_set_level },
	{ "metalog_get_level",      n_metalog_get_level },
	{ "metalog_set_dest",       n_metalog_set_dest },
	{ "metalog_get_dest",       n_metalog_get_dest },
	{ "metalog_set_file",       n_metalog_set_file },
	{ "metalog_set_maxsize",    n_metalog_set_maxsize },
	{ "metalog_flush",          n_metalog_flush },
	{ "metalog_enable",         n_metalog_enable },
	{ "metalog_is_enabled",     n_metalog_is_enabled },
	{ "metalog_diag",           n_metalog_diag },
	{ "metalog_dial_all",       n_metalog_diag },

	// timers
	{ "metalog_tick",           n_metalog_tick },
	{ "metalog_tick_start",     n_metalog_tick_start },
	{ "metalog_tick_stop",      n_metalog_tick_stop },
	{ "metalog_tick_elapsed",   n_metalog_tick_elapsed },
	{ "metalog_tick_reset",     n_metalog_tick_reset },
	{ "metalog_perf",           n_metalog_perf },
	{ "metalog_perf_threshold", n_metalog_perf_threshold },

	{ nullptr, nullptr }
};

// ============================================================
//  Module lifecycle
// ============================================================

void OnAmxxAttach()
{
	g_G.started = time(nullptr);
	ResetAllPlugins();
	MF_AddNatives(metalog_natives);
	ML_OUT("[Metalog] v%s loaded successfully.\n", METALOG_VERSION);
}

void OnPluginsUnloaded()
{
	// plugins are gone (map change) - drop all per-plugin state
	ResetAllPlugins();
}

void OnAmxxDetach()
{
	CloseFile(g_G.fp);
	ResetAllPlugins();
}
