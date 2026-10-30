#include <amxmodx>
#include <cstrike>
#include <fun>

// Change Metalog Off to 0 for debug mode
#define METALOG_OFF 1
#include <metalog>

#define PLUGIN  "Reset Score"
#define VERSION "1.0"
#define AUTHOR  "you"

public plugin_init()
{
	register_plugin(PLUGIN, VERSION, AUTHOR);

	register_clcmd("say /resetscore",      "cmd_resetscore");
	register_clcmd("say_team /resetscore", "cmd_resetscore");

	metalog_plugin_tag("RESETSCORE");
}

public cmd_resetscore(id)
{
	new name[32];
	get_user_name(id, name, charsmax(name));

	// goes ONLY to the log file, so the server console stays clean
	metalog_ex(ML_INFO, MD_FILE, "cmd_resetscore called by %s (id %d)", name, id);

	set_user_frags(id, 0);
	cs_set_user_deaths(id, 0);

	client_print(id, print_chat, "[Reset] Your score was reset.");

	metalog_ex(ML_INFO, MD_FILE, "score reset done for %s", name);
	return PLUGIN_HANDLED;
}
