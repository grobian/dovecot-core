/* Copyright (c) 2017-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#if defined(BUILTIN_LUA) || defined(PLUGIN_BUILD)

#include "llist.h"
#include "istream.h"
#include "array.h"
#include "sha1.h"
#include "hex-binary.h"
#include "strescape.h"
#include "auth.h"
#include "passdb.h"
#include "userdb.h"
#include "auth-request.h"
#include "password-scheme.h"
#include "auth-cache.h"
#include "auth-request-var-expand.h"
#include "settings.h"

#define AUTH_LUA_PASSDB_LOOKUP "auth_passdb_lookup"
#define AUTH_LUA_PASSDB_GET_CACHE_KEY "auth_passdb_get_cache_key"
#define AUTH_LUA_USERDB_LOOKUP "auth_userdb_lookup"
#define AUTH_LUA_USERDB_ITERATE "auth_userdb_iterate"
#define AUTH_LUA_USERDB_GET_CACHE_KEY "auth_userdb_get_cache_key"

#define AUTH_LUA_DOVECOT_AUTH "dovecot_auth"
#define AUTH_LUA_AUTH_REQUEST "auth_request*"

#include "db-lua.h"
#include "dlua-script-private.h"

struct auth_lua_userdb_iterate_context {
	struct userdb_iterate_context ctx;
	pool_t pool;
	unsigned int idx;
	ARRAY_TYPE(const_string) users;
};

#undef DEF
#define DEF(type, name) \
	SETTING_DEFINE_STRUCT_##type(#name, name, struct auth_lua_settings)

static const struct setting_define auth_lua_setting_defines[] = {
	{ .type = SET_FILTER_NAME, .key = "passdb_lua", },
	{ .type = SET_FILTER_NAME, .key = "userdb_lua", },

	SETTING_DEFINE_LIST_END
};

static const struct setting_keyvalue auth_lua_default_settings_keyvalue[] = {
	{ "passdb_lua/passdb_use_worker", "yes"},
	{ "userdb_lua/userdb_use_worker", "yes"},
	{ NULL, NULL }
};

const struct setting_parser_info auth_lua_setting_parser_info = {
	.name = "auth_lua",
#ifndef BUILTIN_LUA
	.plugin_dependency = "auth/libauthdb_lua",
#endif

	.defines = auth_lua_setting_defines,
	.default_settings = auth_lua_default_settings_keyvalue,

	.struct_size = sizeof(struct auth_lua_settings),
	.pool_offset1 = 1 + offsetof(struct auth_lua_settings, pool),
};

static struct auth_request *
auth_lua_check_auth_request(lua_State *L, int arg);

static int
auth_request_lua_do_var_expand(struct auth_request *req, const char *tpl,
			       const char **value_r, const char **error_r)
{
	const char *error;
	if (t_auth_request_var_expand(tpl, req, NULL, value_r, &error) < 0) {
		*error_r = t_strdup_printf("var_expand(%s) failed: %s",
					   tpl, error);
		return -1;
	}
	return 0;
}

static int auth_request_lua_var_expand(lua_State *L)
{
	struct auth_request *req = auth_lua_check_auth_request(L, 1);
	const char *tpl = luaL_checkstring(L, 2);
	const char *value, *error;

	if (auth_request_lua_do_var_expand(req, tpl, &value, &error) < 0) {
		return luaL_error(L, "%s", error);
	} else {
		lua_pushstring(L, value);
	}
	return 1;
}

static int auth_request_lua_log_debug(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	e_debug(authdb_event(request), "db-lua: %s", msg);
	return 0;
}

static int auth_request_lua_log_info(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	e_info(authdb_event(request), "db-lua: %s", msg);
	return 0;
}

static int auth_request_lua_log_warning(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	e_warning(authdb_event(request), "db-lua: %s", msg);
	return 0;
}

static int auth_request_lua_log_error(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *msg = luaL_checkstring(L, 2);
	e_error(authdb_event(request), "db-lua: %s", msg);
	return 0;
}

static int auth_request_lua_passdb(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *key = luaL_checkstring(L, 2);
	lua_pop(L, 1);

	if (request->fields.extra_fields == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushstring(L, auth_fields_find(request->fields.extra_fields, key));
	return 1;
}

static int auth_request_lua_userdb(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *key = luaL_checkstring(L, 2);
	lua_pop(L, 1);

	if (request->fields.userdb_reply == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_pushstring(L, auth_fields_find(request->fields.userdb_reply, key));
	return 1;
}

static int auth_request_lua_password_verify(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	const char *crypted_password = lua_tostring(L, 2);
	const char *scheme;
	const char *plain_password = lua_tostring(L, 3);
	const char *error = NULL;
	const unsigned char *raw_password = NULL;
	size_t raw_password_size;
	int ret;
	if (crypted_password == NULL)
		return luaL_error(L, "Crypted password must not be nil");
	if (plain_password == NULL)
		return luaL_error(L, "Plain password must not be nil");
	struct password_generate_params gen_params = {
		.user = request->fields.original_username,
		.rounds = 0
	};
	scheme = password_get_scheme(&crypted_password);
	if (scheme == NULL)
		scheme = "PLAIN";
	ret = password_decode(crypted_password, scheme,
			      &raw_password, &raw_password_size, &error);
	if (ret <= 0) {
		if (ret < 0) {
			error = t_strdup_printf("Password data is not valid for scheme %s: %s",
						scheme, error);
		} else {
			error = t_strdup_printf("Unknown scheme %s", scheme);
		}
	} else {
		/* Use original_username since it may be important for some
		   password schemes (eg. digest-md5).
		*/
		ret = password_verify(plain_password, &gen_params,
				      scheme, raw_password, raw_password_size, &error);
	}

	lua_pushnumber(L, ret);
	lua_pushstring(L, error);

	return 2;
}

static int auth_request_lua_event(lua_State *L)
{
	struct auth_request *request = auth_lua_check_auth_request(L, 1);
	struct event *event = event_create(authdb_event(request));

	dlua_push_event(L, event);
	event_unref(&event);
	return 1;
}

/* put all methods here */
static const luaL_Reg auth_request_methods[] ={
	{ "var_expand", auth_request_lua_var_expand },
	{ "log_debug", auth_request_lua_log_debug },
	{ "log_info", auth_request_lua_log_info },
	{ "log_warning", auth_request_lua_log_warning },
	{ "log_error", auth_request_lua_log_error },
	{ "password_verify", auth_request_lua_password_verify },
	{ "event", auth_request_lua_event },
	{ NULL, NULL }
};

static int auth_request_lua_index(lua_State *L)
{
	struct auth_request *req = auth_lua_check_auth_request(L, 1);
	const char *key = luaL_checkstring(L, 2);
	lua_pop(L, 1);

	const struct var_expand_table *table =
		auth_request_get_var_expand_table(req);

	/* check if it's variable */
	for(unsigned int i = 0; i < AUTH_REQUEST_VAR_TAB_COUNT; i++) {
		if (null_strcmp(table[i].key, key) == 0) {
			lua_pushstring(L, table[i].value);
			return 1;
		}
	}

	/* check if it's function, then */
	const luaL_Reg *ptr = auth_request_methods;
	while(ptr->name != NULL) {
		if (null_strcmp(key, ptr->name) == 0) {
			lua_pushcfunction(L, ptr->func);
			return 1;
		}
		ptr++;
	}

	lua_pushstring(L, key);
	lua_rawget(L, 1);

	return 1;
}

static void auth_lua_push_auth_request(lua_State *L, struct auth_request *req)
{
	luaL_checkstack(L, 4, "out of memory");
	/* create a table for holding few things */
	lua_createtable(L, 0, 3);
	luaL_setmetatable(L, AUTH_LUA_AUTH_REQUEST);

	lua_pushlightuserdata(L, req);
	lua_setfield(L, -2, "item");

	lua_newtable(L);
	lua_pushlightuserdata(L, req);
	lua_setfield(L, -2, "item");
	luaL_setmetatable(L, "passdb_"AUTH_LUA_AUTH_REQUEST);
	lua_setfield(L, -2, "passdb");

	lua_newtable(L);
	lua_pushlightuserdata(L, req);
	lua_setfield(L, -2, "item");
	luaL_setmetatable(L, "userdb_"AUTH_LUA_AUTH_REQUEST);
	lua_setfield(L, -2, "userdb");

	lua_pushboolean(L, req->fields.skip_password_check);
	lua_setfield(L, -2, "skip_password_check");

#undef LUA_TABLE_SET_BOOL
#define LUA_TABLE_SET_BOOL(field) \
	lua_pushboolean(L, req->field); \
	lua_setfield(L, -2, #field);

	LUA_TABLE_SET_BOOL(passdbs_seen_user_unknown);
	LUA_TABLE_SET_BOOL(passdbs_seen_internal_failure);
	LUA_TABLE_SET_BOOL(userdbs_seen_internal_failure);
}

static struct auth_request *
auth_lua_check_auth_request(lua_State *L, int arg)
{
	if (!lua_istable(L, arg)) {
		(void)luaL_error(L, "Bad argument #%d, expected %s got %s",
				 arg, "auth_request",
				 lua_typename(L, lua_type(L, arg)));
	}
	lua_pushstring(L, "item");
	lua_rawget(L, arg);
	void *bp = (void*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return (struct auth_request*)bp;
}

static void auth_lua_auth_request_register(lua_State *L)
{
	luaL_newmetatable(L, AUTH_LUA_AUTH_REQUEST);
	lua_pushcfunction(L, auth_request_lua_index);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	/* register passdb */
	luaL_newmetatable(L, "passdb_"AUTH_LUA_AUTH_REQUEST);
	lua_pushcfunction(L, auth_request_lua_passdb);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	/* register userdb */
	luaL_newmetatable(L, "userdb_"AUTH_LUA_AUTH_REQUEST);
	lua_pushcfunction(L, auth_request_lua_userdb);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
}

static struct dlua_table_values auth_lua_dovecot_auth_values[] = {
	DLUA_TABLE_ENUM(PASSDB_RESULT_INTERNAL_FAILURE),
	DLUA_TABLE_ENUM(PASSDB_RESULT_SCHEME_NOT_AVAILABLE),
	DLUA_TABLE_ENUM(PASSDB_RESULT_USER_UNKNOWN),
	DLUA_TABLE_ENUM(PASSDB_RESULT_USER_DISABLED),
	DLUA_TABLE_ENUM(PASSDB_RESULT_PASS_EXPIRED),
	DLUA_TABLE_ENUM(PASSDB_RESULT_NEXT),
	DLUA_TABLE_ENUM(PASSDB_RESULT_PASSWORD_MISMATCH),
	DLUA_TABLE_ENUM(PASSDB_RESULT_OK),

	DLUA_TABLE_ENUM(USERDB_RESULT_INTERNAL_FAILURE),
	DLUA_TABLE_ENUM(USERDB_RESULT_USER_UNKNOWN),
	DLUA_TABLE_ENUM(USERDB_RESULT_OK),

	DLUA_TABLE_END
};
static luaL_Reg auth_lua_dovecot_auth_methods[] = {
	{ NULL, NULL }
};

static void auth_lua_dovecot_auth_register(lua_State *L)
{
	dlua_get_dovecot(L);
	/* Create new table for holding values */
	lua_newtable(L);

	/* register constants */
	dlua_set_members(L, auth_lua_dovecot_auth_values, -1);

	/* push new metatable to stack */
	luaL_newmetatable(L, AUTH_LUA_DOVECOT_AUTH);
	/* this will register functions to the metatable itself */
	luaL_setfuncs(L, auth_lua_dovecot_auth_methods, 0);
	/* point __index to self */
	lua_pushvalue(L, -1);
	lua_setfield(L, -1, "__index");
	/* set table's metatable, pops stack */
	lua_setmetatable(L, -2);

	/* put this as "dovecot.auth" */
	lua_setfield(L, -2, "auth");

	/* pop dovecot */
	lua_pop(L, 1);
}

static int auth_lua_call_lookup(lua_State *L, const char *fn, int ret_success,
				struct auth_request *req, const char **error_r)
{
	int err = 0;

	e_debug(authdb_event(req), "Calling %s", fn);

	/* call with auth request as parameter */
	auth_lua_push_auth_request(L, req);
	if (dlua_pcall(L, fn, 1, 2, error_r) < 0)
		return -1;

	if (!lua_isnumber(L, -2)) {
		*error_r = t_strdup_printf("db-lua: %s(req) invalid return value "
					   "(expected number got %s)",
					   fn, luaL_typename(L, -2));
		err = -1;
	} else if (lua_tointeger(L, -2) != ret_success) {
		if (!lua_isstring(L, -1) && !lua_isnil(L, -1)) {
			*error_r = t_strdup_printf(
				"db-lua: %s(req) invalid error return value "
				"(expected nil or string, got %s)",
				fn, luaL_typename(L, -1));
			err = -1;
		}
	} else {
		if (!lua_istable(L, -1) && !lua_isnil(L, -1)) {
			*error_r = t_strdup_printf(
				"db-lua: %s(req) invalid return value "
				"(expected nil or table, got %s)",
				fn, luaL_typename(L, -1));
			err = -1;
		}
	}

	if (err != 0) {
		lua_pop(L, 2);
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	return 0;
}

int auth_lua_script_init(const struct auth_lua_script_parameters *params,
			 const char **error_r)
{
	struct dlua_script *script = params->script;
	dlua_dovecot_register(script);
	auth_lua_dovecot_auth_register(script->L);
	auth_lua_auth_request_register(script->L);
	if (dlua_script_init(script, error_r) < 0)
		return -1;

	return 0;
}

int
auth_lua_script_get_default_cache_key(const struct auth_lua_script_parameters *params,
				      const char **error_r)
{
	const struct auth_passdb_post_settings *post_set;
	struct dlua_script *script = params->script;

	const char *cache_fn;
	switch (params->stype) {
	case AUTH_LUA_SCRIPT_TYPE_PASSDB:
		cache_fn = AUTH_LUA_PASSDB_GET_CACHE_KEY;
		break;
	case AUTH_LUA_SCRIPT_TYPE_USERDB:
		cache_fn = AUTH_LUA_USERDB_GET_CACHE_KEY;
		break;
	default:
		i_unreached();
	}

	if (dlua_script_has_function(script, cache_fn)) {
		if (dlua_pcall(script->L, cache_fn, 0, 1, error_r) < 0)
			return -1;

		if (!lua_isstring(script->L, -1)) {
			*error_r = t_strdup_printf("db-lua: %s invalid return value "
					"(expected string, got %s)",
					cache_fn,
					luaL_typename(script->L, -1));
			return -1;
		}

		if (settings_get(script->event,
				 &auth_passdb_post_setting_parser_info,
				 SETTINGS_GET_FLAG_NO_CHECK |
				 SETTINGS_GET_FLAG_NO_EXPAND,
				 &post_set, error_r) < 0) {
			return -1;
		}

		switch (params->stype) {
		case AUTH_LUA_SCRIPT_TYPE_PASSDB:
			i_assert(params->passdb_module != NULL);
			params->passdb_module->module.default_cache_key =
				auth_cache_parse_key_and_fields(params->pool,
						lua_tostring(script->L, -1),
						&post_set->fields, "lua");
			break;
		case AUTH_LUA_SCRIPT_TYPE_USERDB:
			i_assert(params->userdb_module != NULL);
			params->userdb_module->module.default_cache_key =
				auth_cache_parse_key_and_fields(params->pool,
						lua_tostring(script->L, -1),
						&post_set->fields, "lua");
			break;
		default:
			i_unreached();
		}
		settings_free(post_set);
		lua_pop(script->L, 1);
		lua_gc(script->L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(script->L) == 0);
	}
	return 0;
}

static void auth_lua_export_table(lua_State *L, struct auth_request *req,
				 const char **scheme_r, const char **password_r)
{
	if (lua_isnil(L, -1)) {
		lua_pop(L, 2);
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return;
	}
	i_assert(lua_istable(L, -1));

	lua_pushvalue(L, -1);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		const char *key = t_strdup(lua_tostring(L, -2));
		if (*key == '\0') {
			e_warning(authdb_event(req),
				  "db-lua: Field key cannot be empty - ignoring");
			lua_pop(L, 1);
			continue;
		}
		if (strpbrk(key, "\t\n\r") != NULL) {
			e_warning(authdb_event(req),
				  "db-lua: Field key cannot contain <CR>, <LF> or <TAB> - ignoring");
			lua_pop(L, 1);
			continue;
		}

		const char *value;
		int type = lua_type(L, -1);
		switch(type) {
		case LUA_TNUMBER:
			value = dec2str(lua_tointeger(L, -1));
			break;
		case LUA_TBOOLEAN:
			value = lua_toboolean(L, -1) ? "yes" : "no";
			break;
		case LUA_TSTRING:
			value = t_strdup(lua_tostring(L, -1));
			break;
		case LUA_TNIL:
			value = "";
			break;
		default:
			e_warning(authdb_event(req),
				  "db-lua: '%s' has invalid value type %s - ignoring",
				  key, lua_typename(L, -1));
			value = NULL;
		}

		if (value == NULL) {
			/* do not add */
		} else if (password_r != NULL && strcmp(key, "password") == 0) {
			*scheme_r = password_get_scheme(&value);
			*password_r = value;
		} else if (req->userdb_lookup) {
			auth_request_set_userdb_field(req, key, value);
		} else {
			auth_request_set_field(req, key, value, STATIC_PASS_SCHEME);
		}
		lua_pop(L, 1);
	}

	/* stack has
		key
		table
		passdb_result
	*/
	lua_pop(L, 3);
	lua_gc(L, LUA_GCCOLLECT, 0);
	i_assert(lua_gettop(L) == 0);
}

static enum userdb_result
auth_lua_export_userdb_table(lua_State *L, struct auth_request *req,
			     const char **error_r)
{
	enum userdb_result ret = lua_tointeger(L, -2);

	if (ret != USERDB_RESULT_OK) {
		if (lua_isnil(L, -1))
			*error_r = "userdb failed";
		else
			*error_r = t_strdup(lua_tostring(L, -1));
		lua_pop(L, 2);
		lua_gc(L, LUA_GCCOLLECT, 0);
		return ret;
	}

	auth_lua_export_table(L, req, NULL, NULL);
	return USERDB_RESULT_OK;
}

static enum passdb_result
auth_lua_export_passdb_table(lua_State *L, struct auth_request *req,
			     const char **scheme_r, const char **password_r,
			     const char **error_r)
{
	enum passdb_result ret = lua_tointeger(L, -2);

	if (ret != PASSDB_RESULT_OK) {
		if (lua_isnil(L, -1))
			*error_r = "passdb failed";
		else
			*error_r = t_strdup(lua_tostring(L, -1));
		lua_pop(L, 2);
		lua_gc(L, LUA_GCCOLLECT, 0);
		return ret;
	}

	auth_lua_export_table(L, req, scheme_r, password_r);
	return PASSDB_RESULT_OK;
}

enum passdb_result
auth_lua_call_password_verify(struct dlua_script *script,
			      struct auth_request *req, const char *password, const char **error_r)
{
	lua_State *L = script->L;
	int err = 0;

	e_debug(authdb_event(req), "Calling %s", AUTH_LUA_PASSWORD_VERIFY);

	/* call with auth request, password as parameters */
	auth_lua_push_auth_request(L, req);
	lua_pushstring(L, password);

	if (dlua_pcall(L, AUTH_LUA_PASSWORD_VERIFY, 2, 2, error_r) < 0)
		return PASSDB_RESULT_INTERNAL_FAILURE;

	if (!lua_isnumber(L, -2)) {
		*error_r = t_strdup_printf("db-lua: %s invalid return value "
					   "(expected number got %s)",
					   AUTH_LUA_PASSWORD_VERIFY,
					   luaL_typename(L, -2));
		err = -1;
	} else if (lua_tointeger(L, -2) != PASSDB_RESULT_OK) {
		if (!lua_isstring(L, -1) && !lua_isnil(L, -1)) {
			*error_r = t_strdup_printf(
				"db-lua: %s invalid error return value "
				"(expected nil or string, got %s)",
				AUTH_LUA_PASSWORD_VERIFY, luaL_typename(L, -1));
			err = -1;
		}
	} else {
		if (!lua_istable(L, -1) && !lua_isnil(L, -1)) {
			*error_r = t_strdup_printf(
				"db-lua: %s invalid return value "
				"(expected nil or table, got %s)",
				AUTH_LUA_PASSWORD_VERIFY, luaL_typename(L, -1));
			err = -1;
		}
	}

	if (err != 0) {
		lua_pop(L, 2);
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	return auth_lua_export_passdb_table(L, req, NULL, NULL, error_r);
}


enum passdb_result
auth_lua_call_passdb_lookup(struct dlua_script *script,
			    struct auth_request *req, const char **scheme_r,
			    const char **password_r, const char **error_r)
{
	lua_State *L = script->L;

	*scheme_r = *password_r = NULL;
	if (auth_lua_call_lookup(L, AUTH_LUA_PASSDB_LOOKUP, PASSDB_RESULT_OK,
				 req, error_r) < 0) {
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	return auth_lua_export_passdb_table(L, req, scheme_r, password_r,
					    error_r);
}


enum userdb_result
auth_lua_call_userdb_lookup(struct dlua_script *script,
			    struct auth_request *req, const char **error_r)
{
	lua_State *L = script->L;

	if (auth_lua_call_lookup(L, AUTH_LUA_USERDB_LOOKUP, USERDB_RESULT_OK,
				 req, error_r) < 0) {
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return USERDB_RESULT_INTERNAL_FAILURE;
	}

	return auth_lua_export_userdb_table(L, req, error_r);
}

struct userdb_iterate_context *
auth_lua_call_userdb_iterate_init(struct dlua_script *script, struct auth_request *req,
				  userdb_iter_callback_t *callback, void *context)
{
	lua_State *L = script->L;

	pool_t pool = pool_alloconly_create(MEMPOOL_GROWING"lua userdb iterate", 128);
	struct auth_lua_userdb_iterate_context *actx =
		p_new(pool, struct auth_lua_userdb_iterate_context, 1);

	actx->pool = pool;
	actx->ctx.auth_request = req;
	actx->ctx.callback = callback;
	actx->ctx.context = context;

	if (!dlua_script_has_function(script, AUTH_LUA_USERDB_ITERATE)) {
		actx->ctx.failed = TRUE;
		return &actx->ctx;
	}

	e_debug(authdb_event(req), "Calling %s", AUTH_LUA_USERDB_ITERATE);

	const char *error;
	if (dlua_pcall(L, AUTH_LUA_USERDB_ITERATE, 0, 1, &error) < 0) {
		e_error(authdb_event(req),
			"db-lua: " AUTH_LUA_USERDB_ITERATE " failed: %s",
			error);
		actx->ctx.failed = TRUE;
		return &actx->ctx;
	}

	if (!lua_istable(L, -1)) {
		e_error(authdb_event(req),
			"db-lua: Cannot iterate, return value is not table");
		actx->ctx.failed = TRUE;
		lua_pop(L, 1);
		lua_gc(L, LUA_GCCOLLECT, 0);
		i_assert(lua_gettop(L) == 0);
		return &actx->ctx;
	}

	p_array_init(&actx->users, pool, 8);

	/* stack is now
		table */

	/* see lua_next documentation */
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		/* stack is now
			value
			key
			table */
		if (!lua_isstring(L, -1)) {
			e_error(authdb_event(req),
				"db-lua: Value is not string");
			actx->ctx.failed = TRUE;
			lua_pop(L, 3);
			lua_gc(L, LUA_GCCOLLECT, 0);
			i_assert(lua_gettop(L) == 0);
			return &actx->ctx;
		}
		const char *str = p_strdup(pool, lua_tostring(L, -1));
		array_push_back(&actx->users, &str);
		lua_pop(L, 1);
		/* stack is now
			key
			table */
	}

	/* stack is now
		table
	*/

	lua_pop(L, 1);
	lua_gc(L, LUA_GCCOLLECT, 0);
	i_assert(lua_gettop(L) == 0);

	return &actx->ctx;
}

void auth_lua_userdb_iterate_next(struct userdb_iterate_context *ctx)
{
	struct auth_lua_userdb_iterate_context *actx =
		container_of(ctx, struct auth_lua_userdb_iterate_context, ctx);

	if (ctx->failed || actx->idx >= array_count(&actx->users)) {
		ctx->callback(NULL, ctx->context);
		return;
	}

	const char *user = array_idx_elem(&actx->users, actx->idx++);
	ctx->callback(user, ctx->context);
}

int auth_lua_userdb_iterate_deinit(struct userdb_iterate_context *ctx)
{
	struct auth_lua_userdb_iterate_context *actx =
		container_of(ctx, struct auth_lua_userdb_iterate_context, ctx);

	int ret = ctx->failed ? -1 : 0;
	pool_unref(&actx->pool);
	return ret;
}

#ifndef BUILTIN_LUA
/* Building a plugin */
extern struct passdb_module_interface passdb_lua_plugin;
extern struct userdb_module_interface userdb_lua_plugin;

void authdb_lua_init(void);
void authdb_lua_deinit(void);

void authdb_lua_init(void)
{
	passdb_register_module(&passdb_lua_plugin);
	userdb_register_module(&userdb_lua_plugin);

}
void authdb_lua_deinit(void)
{
	passdb_unregister_module(&passdb_lua_plugin);
	userdb_unregister_module(&userdb_lua_plugin);
}
#endif

#endif
