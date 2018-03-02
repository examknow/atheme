/*
 * Copyright (c) 2005-2006 William Pitcock, et al.
 * Rights to this code are as documented in doc/LICENSE.
 *
 * This file contains code for the NickServ IDENTIFY and LOGIN functions.
 */

#include "atheme.h"

// Check whether we are compiling IDENTIFY or LOGIN
#ifdef NICKSERV_LOGIN
#define COMMAND_UC "LOGIN"
#define COMMAND_LC "login"
#else
#define COMMAND_UC "IDENTIFY"
#define COMMAND_LC "identify"
#endif

static void
ns_cmd_login(struct sourceinfo *si, int parc, char *parv[])
{
	struct user *u = si->su;
	struct myuser *mu;
	mowgli_node_t *n, *tn;
	const char *target = parv[0];
	const char *password = parv[1];
	char lau[BUFSIZE];
	hook_user_login_check_t req;

	if (si->su == NULL)
	{
		command_fail(si, fault_noprivs, _("\2%s\2 can only be executed via IRC."), COMMAND_UC);
		return;
	}

#ifndef NICKSERV_LOGIN
	if (!nicksvs.no_nick_ownership && target && !password)
	{
		password = target;
		target = si->su->nick;
	}
#endif

	if (!target || !password)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, COMMAND_UC);
		command_fail(si, fault_needmoreparams, nicksvs.no_nick_ownership ? "Syntax: " COMMAND_UC " <account> <password>" : "Syntax: " COMMAND_UC " [nick] <password>");
		return;
	}

	mu = myuser_find_by_nick(target);
	if (!mu)
	{
		command_fail(si, fault_nosuch_target, _("\2%s\2 is not a registered nickname."), target);
		return;
	}

	req.si = si;
	req.mu = mu;
	req.allowed = true;
	hook_call_user_can_login(&req);
	if (!req.allowed)
	{
		command_fail(si, fault_authfail, nicksvs.no_nick_ownership ? "You cannot log in as \2%s\2 because the server configuration disallows it."
									   : "You cannot identify to \2%s\2 because the server configuration disallows it.", entity(mu)->name);
		logcommand(si, CMDLOG_LOGIN, "failed " COMMAND_UC " to \2%s\2 (denied by hook)", entity(mu)->name);
		return;
	}

	if (metadata_find(mu, "private:freeze:freezer"))
	{
		command_fail(si, fault_authfail, nicksvs.no_nick_ownership ? "You cannot log in as \2%s\2 because the account has been frozen."
									   : "You cannot identify to \2%s\2 because the nickname has been frozen.", entity(mu)->name);
		logcommand(si, CMDLOG_LOGIN, "failed " COMMAND_UC " to \2%s\2 (frozen)", entity(mu)->name);
		return;
	}

	if (mu->flags & MU_NOPASSWORD)
	{
		command_fail(si, fault_authfail, _("Password authentication is disabled for this account."));
		logcommand(si, CMDLOG_LOGIN, "failed " COMMAND_UC " to \2%s\2 (password authentication disabled)", entity(mu)->name);
		return;
	}

	if (u->myuser == mu)
	{
		command_fail(si, fault_nochange, _("You are already logged in as \2%s\2."), entity(u->myuser)->name);
		if (mu->flags & MU_WAITAUTH)
			command_fail(si, fault_nochange, _("Please check your email for instructions to complete your registration."));
		return;
	}
	else if (u->myuser != NULL && !command_find(si->service->commands, "LOGOUT"))
	{
		command_fail(si, fault_alreadyexists, _("You are already logged in as \2%s\2."), entity(u->myuser)->name);
		return;
	}

	if (verify_password(mu, password))
	{
		if (MOWGLI_LIST_LENGTH(&mu->logins) >= me.maxlogins)
		{
			command_fail(si, fault_toomany, _("There are already \2%zu\2 sessions logged in to \2%s\2 (maximum allowed: %u)."), MOWGLI_LIST_LENGTH(&mu->logins), entity(mu)->name, me.maxlogins);
			lau[0] = '\0';
			MOWGLI_ITER_FOREACH(n, mu->logins.head)
			{
				if (lau[0] != '\0')
					mowgli_strlcat(lau, ", ", sizeof lau);
				mowgli_strlcat(lau, ((struct user *)n->data)->nick, sizeof lau);
			}
			command_fail(si, fault_toomany, _("Logged in nicks are: %s"), lau);
			logcommand(si, CMDLOG_LOGIN, "failed " COMMAND_UC " to \2%s\2 (too many logins)", entity(mu)->name);
			return;
		}

		// if they are identified to another account, nuke their session first
		if (u->myuser)
		{
			command_success_nodata(si, _("You have been logged out of \2%s\2."), entity(u->myuser)->name);

			if (ircd_on_logout(u, entity(u->myuser)->name))
				// logout killed the user...
				return;
		        u->myuser->lastlogin = CURRTIME;
		        MOWGLI_ITER_FOREACH_SAFE(n, tn, u->myuser->logins.head)
		        {
			        if (n->data == u)
		                {
		                        mowgli_node_delete(n, &u->myuser->logins);
		                        mowgli_node_free(n);
		                        break;
		                }
		        }
		        u->myuser = NULL;
		}

		command_success_nodata(si, nicksvs.no_nick_ownership ? _("You are now logged in as \2%s\2.") : _("You are now identified for \2%s\2."), entity(mu)->name);

		if (!(mu->flags & MU_CRYPTPASS))
			(void) command_success_nodata(si, "%s", _("Warning: Your password is not encrypted."));

		myuser_login(si->service, u, mu, true);
		logcommand(si, CMDLOG_LOGIN, COMMAND_UC);

		return;
	}

	logcommand(si, CMDLOG_LOGIN, "failed " COMMAND_UC " to \2%s\2 (bad password)", entity(mu)->name);

	command_fail(si, fault_authfail, _("Invalid password for \2%s\2."), entity(mu)->name);
	bad_password(si, mu);
}

#ifdef NICKSERV_LOGIN
static struct command ns_login = { COMMAND_UC, N_("Authenticates to a services account."), AC_NONE, 2, ns_cmd_login, { .path = "nickserv/" COMMAND_LC } };
#else
static struct command ns_login = { COMMAND_UC, N_("Identifies to services for a nickname."), AC_NONE, 2, ns_cmd_login, { .path = "nickserv/" COMMAND_LC } };
#endif

static void
mod_init(struct module ATHEME_VATTR_UNUSED *const restrict m)
{
	service_named_bind_command("nickserv", &ns_login);

	hook_add_event("user_can_login");
}

static void
mod_deinit(const enum module_unload_intent ATHEME_VATTR_UNUSED intent)
{
	service_named_unbind_command("nickserv", &ns_login);
}

SIMPLE_DECLARE_MODULE_V1("nickserv/" COMMAND_LC, MODULE_UNLOAD_CAPABILITY_OK)
