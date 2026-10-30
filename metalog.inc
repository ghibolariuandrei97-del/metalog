#if defined _metalog_included
	#endinput
#endif
#define _metalog_included

// ============================================================
//  Metalog v2.0.0 - Advanced Debugging Module for AMX Mod X
// ============================================================
//
//  COMPILE-TIME SWITCH
//  Set to 1 -> all metalog_* calls become empty (zero overhead)
//  Set to 0 -> real natives from the module are used
//
//  You can change it here (affects every plugin), or per plugin
//  by defining it BEFORE the include:
//
//      #define METALOG_OFF 1
//      #include <metalog>
//
#if !defined METALOG_OFF
	#define METALOG_OFF 0
#endif

#pragma reqlib metalog
#if !defined AMXMODX_NOAUTOLOAD
	#pragma loadlib metalog
#endif

// ============================================================
//  Log Levels (bitflags)
// ============================================================
enum MetalogLevel
{
	ML_GLOBAL = -1,		// only for metalog_plugin_level(): follow global level
	ML_NONE   = 0,
	ML_ERROR  = 1,		// Important errors
	ML_WARN   = 2,		// Warnings
	ML_INFO   = 4,		// General information
	ML_DEBUG  = 8,		// Normal debug
	ML_TRACE  = 16,		// Very detailed trace
	ML_PERF   = 32,		// Performance / timing
	ML_ALL    = 63		// Everything
};

// ============================================================
//  Destinations
// ============================================================
enum MetalogDest
{
	MD_NONE    = 0,
	MD_SERVER  = 1,		// Server console
	MD_CLIENTS = 2,		// reserved - use the metalog_chat stock
	MD_FILE    = 4,		// Log file
	MD_CHAT    = 8,		// reserved - use the metalog_chat stock
	MD_AMXX    = 16,	// AMXX log (log_amx file)
	MD_DEFAULT = MD_SERVER | MD_FILE
};

#if METALOG_OFF > 0

	// Empty function used when debugging is compiled out
	stock metalog_null() {}

	// All calls become empty -> zero overhead
	// (arguments are NOT evaluated when compiled out)
	#define metalog(%1)                  	metalog_null()
	#define metalog_ex(%1)               	metalog_null()
	#define metalog_tag(%1)              	metalog_null()
	#define metalog_once(%1)             	metalog_null()
	#define metalog_every(%1)            	metalog_null()
	#define metalog_assert(%1)           	true
	#define metalog_dump(%1)             	metalog_null()
	#define metalog_chat(%1)             	metalog_null()

	// per plugin (runtime off switch, dial, tag ...)
	#define metalog_off(%1)              	metalog_null()
	#define metalog_is_off()             	true
	#define metalog_plugin_level(%1)     	metalog_null()
	#define metalog_plugin_tag(%1)       	metalog_null()
	#define metalog_plugin_file(%1)      	metalog_null()
	#define metalog_dial()               	metalog_null()
	#define metalog_dial(%1)             	metalog_null()
	#define metalog_get_count()          	0
	#define metalog_get_count(%1)        	0
	#define metalog_clear()              	metalog_null()

	// global config
	#define metalog_set_level(%1)        	metalog_null()
	#define metalog_get_level()          	metalog_null()
	#define metalog_set_dest(%1)         	metalog_null()
	#define metalog_get_dest()           	metalog_null()
	#define metalog_set_file(%1)         	metalog_null()
	#define metalog_set_maxsize(%1)      	metalog_null()
	#define metalog_flush()              	metalog_null()
	#define metalog_enable(%1)           	metalog_null()
	#define metalog_is_enabled()         	false
	#define metalog_diag()               	metalog_null()
	#define metalog_dial_all()           	metalog_null()

	// timers
	#define metalog_tick()               	0.0
	#define metalog_tick_start()         	metalog_null()
	#define metalog_tick_start(%1)       	metalog_null()
	#define metalog_tick_stop()          	0.0
	#define metalog_tick_stop(%1)        	0.0
	#define metalog_tick_elapsed()       	0.0
	#define metalog_tick_elapsed(%1)     	0.0
	#define metalog_tick_reset()         	metalog_null()
	#define metalog_tick_reset(%1)       	metalog_null()
	#define metalog_perf()               	0.0
	#define metalog_perf(%1)             	0.0
	#define metalog_perf_threshold(%1)   	metalog_null()

#else

	// --------------------------------------------------------
	//  LOGGING
	// --------------------------------------------------------

	/**
	 * Basic log
	 * Example: metalog(ML_DEBUG, "Player %d died", id);
	 */
	native metalog(MetalogLevel:level, const message[], any:...);

	/**
	 * Log with explicit destinations
	 * Example: metalog_ex(ML_ERROR, MD_SERVER | MD_FILE, "Critical error: %s", reason);
	 */
	native metalog_ex(MetalogLevel:level, MetalogDest:dest, const message[], any:...);

	/**
	 * Log with custom tag
	 * Example: metalog_tag(ML_INFO, "CONNECT", "id=%d name=%s", id, name);
	 */
	native metalog_tag(MetalogLevel:level, const tag[], const message[], any:...);

	/**
	 * Log only once (per plugin) based on a unique key
	 * Example: metalog_once(ML_WARN, "first_spawn", "First spawn detected");
	 */
	native metalog_once(MetalogLevel:level, const key[], const message[], any:...);

	/**
	 * Log on the 1st call, then once every N calls of that message
	 * Example: metalog_every(ML_TRACE, 50, "Called 50 times");
	 */
	native metalog_every(MetalogLevel:level, count, const message[], any:...);

	/**
	 * Logs an ERROR when the condition is false. Returns the condition.
	 * Example: metalog_assert(id > 0, "Invalid id %d", id);
	 */
	native bool:metalog_assert(bool:condition, const message[], any:...);

	/**
	 * Logs the first 32 cells of an array.
	 * Example: metalog_dump(ML_DEBUG, "data", data, sizeof data);
	 */
	native metalog_dump(MetalogLevel:level, const label[], const any:data[], size);

	// --------------------------------------------------------
	//  PER-PLUGIN CONTROL (applies only to the calling plugin)
	// --------------------------------------------------------

	/**
	 * RUNTIME switch for the calling plugin only.
	 * metalog_off(1) = mute Metalog for this plugin
	 * metalog_off(0) = enable it again
	 * (not to be confused with the compile-time METALOG_OFF define)
	 */
	native metalog_off(off);

	/** Is Metalog muted for this plugin? */
	native bool:metalog_is_off();

	/** Own level mask for this plugin. ML_GLOBAL = follow the global level. */
	native metalog_plugin_level(MetalogLevel:level);

	/** Permanent tag shown on every line from this plugin. */
	native metalog_plugin_tag(const tag[]);

	/** Own log file for this plugin: logs/metalog/<plugin>.log */
	native metalog_plugin_file(bool:enable);

	/**
	 * Prints info ONLY about the calling plugin to the server console:
	 * state, level, tag, file, counters, timers, last N entries.
	 * Prints nothing (returns 0) if metalog_off(1) is active, unless force = true.
	 * Example: metalog_dial();
	 */
	native metalog_dial(history = 8, bool:force = false);

	/** How many messages this plugin logged (by level mask). */
	native metalog_get_count(MetalogLevel:level = ML_ALL);

	/** Clears stats, history and once/every counters of this plugin. */
	native metalog_clear();

	// --------------------------------------------------------
	//  GLOBAL CONFIGURATION
	// --------------------------------------------------------
	native metalog_set_level(MetalogLevel:level);
	native MetalogLevel:metalog_get_level();
	native metalog_set_dest(MetalogDest:dest);
	native MetalogDest:metalog_get_dest();
	native metalog_set_file(const path[]);
	native metalog_set_maxsize(kb);                 // log rotation limit, 0 = off
	native metalog_flush();
	native metalog_enable(bool:enable = true);
	native bool:metalog_is_enabled();

	/**
	 * Global report: module status + table with every plugin using Metalog.
	 * Example: metalog_diag();
	 */
	native metalog_diag();
	native metalog_dial_all();

	// --------------------------------------------------------
	//  HIGH-RESOLUTION TIMERS (ids 0-31, separate per plugin)
	// --------------------------------------------------------
	native Float:metalog_tick();                    // current time in seconds
	native metalog_tick_start(id = 0);              // start timer
	native Float:metalog_tick_stop(id = 0);         // stop and return elapsed time
	native Float:metalog_tick_elapsed(id = 0);      // peek elapsed time
	native metalog_tick_reset(id = 0);

	/** Stops the timer and logs "<label> took X ms" as PERF. Returns seconds. */
	native Float:metalog_perf(id = 0, const label[] = "");

	/** Only log metalog_perf() results slower than this (ms). 0 = always. */
	native metalog_perf_threshold(Float:ms);

	// --------------------------------------------------------
	//  STOCKS
	// --------------------------------------------------------

	/** Logs normally and also prints in chat to admins (needs amxmodx.inc). */
	stock metalog_chat(MetalogLevel:level, const fmt[], any:...)
	{
		static msg[191];
		vformat(msg, charsmax(msg), fmt, 3);

		metalog(level, "%s", msg);

		for (new id = 1; id <= get_maxplayers(); id++)
		{
			if (is_user_connected(id) && (get_user_flags(id) & ADMIN_RCON))
				client_print(id, print_chat, "[Metalog] %s", msg);
		}
	}

#endif

// ============================================================
//  Recommended short macros
// ============================================================

#define ML_LOG(%1)          metalog(ML_INFO, %1)
#define ML_INFO(%1)         metalog(ML_INFO, %1)
#define ML_ERROR(%1)        metalog(ML_ERROR, %1)
#define ML_WARN(%1)         metalog(ML_WARN, %1)
#define ML_DEBUG(%1)        metalog(ML_DEBUG, %1)
#define ML_TRACE(%1)        metalog(ML_TRACE, %1)
#define ML_PERF(%1)         metalog(ML_PERF, %1)

#define ML_TIMER_START(%1)  metalog_tick_start(%1)
#define ML_TIMER_STOP(%1)   metalog_tick_stop(%1)
