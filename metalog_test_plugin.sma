#include <amxmodx>
// Uncomment to test the compile-time switch (all metalog calls become empty):
// #define METALOG_OFF 1
#include <metalog>

#define PLUGIN  "Metalog Test"
#define VERSION "1.0"
#define AUTHOR  "you"

new g_iLoops;

public plugin_init()
{
	register_plugin(PLUGIN, VERSION, AUTHOR);

	// per-plugin setup
	metalog_plugin_tag("TEST");
	metalog_plugin_level(ML_ALL);          // this plugin logs every level
	metalog_perf_threshold(0.0);           // 0 = always log perf results

	// server console commands (type them in the server console / rcon)
	register_srvcmd("mt_off",   "cmd_off");     // mt_off 1  /  mt_off 0
	register_srvcmd("mt_dial",  "cmd_dial");    // dial only for THIS plugin
	register_srvcmd("mt_force", "cmd_force");   // dial even if off
	register_srvcmd("mt_all",   "cmd_all");     // global report
	register_srvcmd("mt_test",  "cmd_test");    // writes a bit of everything
	register_srvcmd("mt_perf",  "cmd_perf");    // timer + PERF log
	register_srvcmd("mt_file",  "cmd_file");    // mt_file 1 / 0 : own log file
	register_srvcmd("mt_clear", "cmd_clear");

	metalog(ML_INFO, "Plugin initialised (%s v%s)", PLUGIN, VERSION);
}

public cmd_off()
{
	new arg[8];
	read_argv(1, arg, charsmax(arg));
	new off = str_to_num(arg);

	// message first, so you can see it before muting
	if (off)
		metalog(ML_INFO, "Muting Metalog for this plugin...");

	metalog_off(off);

	if (!off)
		metalog(ML_INFO, "Metalog is back ON for this plugin");

	server_print("[TEST] metalog_off(%d) -> is_off = %d", off, metalog_is_off());
	return PLUGIN_HANDLED;
}

public cmd_dial()
{
	// prints only this plugin's info; prints nothing if metalog_off(1)
	if (!metalog_dial(8))
		server_print("[TEST] metalog_dial() printed nothing (Metalog is OFF for this plugin)");

	return PLUGIN_HANDLED;
}

public cmd_force()
{
	metalog_dial(8, true);
	return PLUGIN_HANDLED;
}

public cmd_all()
{
	metalog_dial_all();
	return PLUGIN_HANDLED;
}

public cmd_test()
{
	metalog(ML_ERROR, "This is an ERROR");
	metalog(ML_WARN,  "This is a WARN");
	metalog(ML_INFO,  "Formatted INFO: %d + %d = %d", 2, 3, 5);
	metalog(ML_DEBUG, "DEBUG with a string: %s", "hello");
	metalog(ML_TRACE, "TRACE message");

	metalog_tag(ML_INFO, "CUSTOM", "Message with its own tag");
	metalog_once(ML_INFO, "once_key", "This line prints only ONCE per plugin");
	metalog_assert(1 == 2, "Math is broken (expected, this is a test)");

	new data[5] = {10, 20, 30, 40, 50};
	metalog_dump(ML_DEBUG, "data", data, sizeof data);

	// metalog_every: logs 1st call, then every 3rd call
	for (new i = 0; i < 7; i++)
		metalog_every(ML_INFO, 3, "every-3 message (loop)");

	g_iLoops++;
	server_print("[TEST] mt_test done (run #%d). Errors so far: %d", g_iLoops, metalog_get_count(ML_ERROR));
	return PLUGIN_HANDLED;
}

public cmd_perf()
{
	metalog_tick_start(0);

	// burn a little time
	new sum;
	for (new i = 0; i < 200000; i++)
		sum += i % 7;

	new Float:sec = metalog_perf(0, "busy loop");
	server_print("[TEST] loop result %d, took %.6f sec", sum, sec);
	return PLUGIN_HANDLED;
}

public cmd_file()
{
	new arg[8];
	read_argv(1, arg, charsmax(arg));
	metalog_plugin_file(str_to_num(arg) != 0);
	metalog(ML_INFO, "Own log file is now %s", str_to_num(arg) ? "ENABLED" : "DISABLED");
	return PLUGIN_HANDLED;
}

public cmd_clear()
{
	metalog_clear();
	server_print("[TEST] stats and history cleared");
	return PLUGIN_HANDLED;
}
