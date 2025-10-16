
#include "mod_kroko_transcribe.h"
#include "kroko_transcribe_glue.h"

/* Prototypes */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kroko_transcribe_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_kroko_transcribe_load);

SWITCH_MODULE_DEFINITION(mod_kroko_transcribe, mod_kroko_transcribe_load, mod_kroko_transcribe_shutdown, NULL);

static switch_status_t do_stop(switch_core_session_t *session, char* bugname);
mod_kroko_transcribe_globals_t mod_kroko_transcribe_globals;

static void responseHandler(switch_core_session_t* session, 
	const char* eventName, const char * json, const char* bugname, int finished) {
	switch_event_t *event;
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
	switch_channel_event_set_data(channel, event);
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-vendor", "kroko");
	switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "transcription-session-finished", finished ? "true" : "false");
	if (finished) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "responseHandler returning event %s, from finished recognition session\n", eventName);
	}
	if (json) switch_event_add_body(event, "%s", json);
	if (bugname) switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "media-bugname", bugname);
	switch_event_fire(&event);
}

static switch_bool_t capture_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
	switch_core_session_t *session = switch_core_media_bug_get_session(bug);

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_INIT.\n");
		break;

	case SWITCH_ABC_TYPE_CLOSE:
		{
			private_t *tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_CLOSE.\n");

			kroko_transcribe_session_stop(session, 1,  tech_pvt->bugname);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Finished SWITCH_ABC_TYPE_CLOSE.\n");
		}
		break;
	
	case SWITCH_ABC_TYPE_READ:

		return kroko_transcribe_frame(session, bug);
		break;

	case SWITCH_ABC_TYPE_WRITE:
	default:
		break;
	}

	return SWITCH_TRUE;
}

static switch_status_t start_capture(switch_core_session_t *session, switch_media_bug_flag_t flags, 
  char* lang, int interim, char* bugname)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug;
	switch_status_t status;
	void *pUserData;

	void *vptr = NULL;
	mod_kroko_transcribe_conn_t *ptr = NULL;

	if (mod_kroko_transcribe_globals.profiles != NULL) {
		vptr = switch_core_hash_find(mod_kroko_transcribe_globals.profiles,lang);
		if (vptr != NULL) {
			ptr = (mod_kroko_transcribe_conn_t *)vptr;
			if (ptr->channel_mode > 0) {
				flags = SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_STEREO;
			}
		}
	}

	if (switch_channel_get_private(channel, MY_BUG_NAME)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "removing bug from previous transcribe\n");
		do_stop(session, bugname);
	}

	if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
		return SWITCH_STATUS_FALSE;
	}

	if (SWITCH_STATUS_FALSE == kroko_transcribe_session_init(session, responseHandler, flags & SMBF_STEREO ? 2 : 1, lang, interim, bugname, &pUserData)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error initializing kroko speech session.\n");
		return SWITCH_STATUS_FALSE;
	}
	if ((status = switch_core_media_bug_add(session, "kroko_transcribe", NULL, capture_callback, pUserData, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
		return status;
	}
	switch_channel_set_private(channel, MY_BUG_NAME, bug);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "added media bug for kroko transcribe\n");
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "checked media bug flags: read=%s, write=%s\n",
						switch_test_flag(bug, SMBF_READ_STREAM) ? "yes" : "no",
						switch_test_flag(bug, SMBF_WRITE_STREAM) ? "yes" : "no");

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t do_stop(switch_core_session_t *session,  char* bugname)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_media_bug_t *bug = switch_channel_get_private(channel, MY_BUG_NAME);

	if (bug) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Received user command command to stop transcribe.\n");
		status = kroko_transcribe_session_stop(session, 0, bugname);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "stopped transcribe.\n");
	}

	return status;
}

void mod_kroko_init_conn(mod_kroko_transcribe_conn_t *conn) {
	if (conn != NULL) {
		conn->kroko_asr_hostname = NULL;
		conn->kroko_asr_port = 0;
		conn->prot = wss;
		conn->sample_rate = 16000;
		conn->callback_url = NULL;
		conn->channel_mode = 1;
		conn->result_mode = 0;
	}
}

void mod_kroko_print_profile(char *name, mod_kroko_transcribe_conn_t *read_cfg){
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"name: %s, host: %s, port: %d, prot: %d, sample_rate: %d; cb_url: %s, ch_mode: %d, res_mode: %d\n",
		name, read_cfg->kroko_asr_hostname, read_cfg->kroko_asr_port, read_cfg->prot, read_cfg->sample_rate, 
		read_cfg->callback_url , read_cfg->channel_mode, read_cfg->result_mode);
}

static switch_status_t mod_kroko_transcribe_create_profile(char *profile_name, mod_kroko_transcribe_conn_t *read_cfg) {
	mod_kroko_transcribe_conn_t *conn = NULL;
	switch_memory_pool_t *pool = NULL;
	
	switch_core_new_memory_pool(&pool);
	conn = switch_core_alloc(pool, sizeof(mod_kroko_transcribe_conn_t));

	if (conn != NULL) {
		conn->kroko_asr_hostname = read_cfg->kroko_asr_hostname;
		conn->kroko_asr_port = read_cfg->kroko_asr_port ? read_cfg->kroko_asr_port : 6006;
		conn->prot = read_cfg->prot;
		conn->sample_rate = read_cfg->sample_rate;
		conn->callback_url = read_cfg->callback_url;
		conn->channel_mode = read_cfg->channel_mode;
		conn->result_mode = read_cfg->result_mode;

		switch_core_hash_insert(mod_kroko_transcribe_globals.profiles, profile_name, (void *) conn);
	} else {
		return SWITCH_STATUS_GENERR;
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t mod_kroko_transcribe_load_cfg() {
	char *conf = "kroko_transcribe.conf";
	switch_xml_t xml, cfg, profiles, profile, param, sys;
	mod_kroko_transcribe_conn_t read_cfg;

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Loading a kroko module config\n");

	if (!(xml = switch_xml_open_cfg(conf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", conf);
		return SWITCH_STATUS_GENERR;
	}

	for (sys = switch_xml_child(cfg, "system"); sys; sys = sys->next) {
		for (param = switch_xml_child(sys, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			if ( ! strncmp(var, "debug", 5) ) {
				mod_kroko_transcribe_globals.debug = atoi(switch_xml_attr_soft(param, "value"));
			}
		}

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "debug: %d;\n",mod_kroko_transcribe_globals.debug);
	}

	if ( (profiles = switch_xml_child(cfg, "profiles")) != NULL) {
		for (profile = switch_xml_child(profiles, "profile"); profile; profile = profile->next) {
			char *name=NULL, *tmp=NULL;
			mod_kroko_init_conn(&read_cfg);

			name = (char *)switch_xml_attr_soft(profile, "name");

			for (param = switch_xml_child(profile, "param"); param; param = param->next) {
				char *var = (char *) switch_xml_attr_soft(param, "name");
				if ( ! strncmp(var, "host", 4) ) {
					read_cfg.kroko_asr_hostname = (char *) switch_xml_attr_soft(param, "value");
				} else if ( ! strncmp(var, "port", 4) ) {
					read_cfg.kroko_asr_port = atoi(switch_xml_attr_soft(param, "value"));
				} else if ( ! strncmp(var, "sample_rate", 10) ) {
					read_cfg.sample_rate = atoi(switch_xml_attr_soft(param, "value"));
				} else if ( ! strncmp(var, "prot", 4) ) {
					tmp = (char *) switch_xml_attr_soft(param, "value");
					if (strcmp(tmp,"ws")==0) {
						read_cfg.prot = ws;
					} else if (strcmp(tmp,"wss")==0) {
						read_cfg.prot = wss;
					} else {
						read_cfg.prot = wss;
					}
				} else if ( ! strncmp(var, "callback_url", 12) ) {
					read_cfg.callback_url = (char *) switch_xml_attr_soft(param, "value");
				} else if ( ! strncmp(var, "catched_channels", 16) ) {
					tmp = (char *) switch_xml_attr_soft(param, "value");
					if (strcmp(tmp,"ro")==0) {
						read_cfg.channel_mode = read_mode;
					} else if (strcmp(tmp,"wo")==0) {
						read_cfg.channel_mode = write_mode;
					} else if (strcmp(tmp,"rw")==0) {
						read_cfg.channel_mode = rw_mode;
					}
				} else if ( ! strncmp(var, "result_mode", 11)) {
					tmp = (char *) switch_xml_attr_soft(param, "value");
					if (strcmp(tmp,"text")==0) {
						read_cfg.result_mode = text;
					} else if (strcmp(tmp,"json")==0) {
						read_cfg.result_mode = json;
					}
				}
			}

			if(mod_kroko_transcribe_globals.debug) {
				mod_kroko_print_profile( name, &read_cfg);
			}

			mod_kroko_transcribe_create_profile(name, &read_cfg);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

#define TRANSCRIBE_API_SYNTAX "<uuid> [start|stop] lang-code [interim] [stereo|mono]"
SWITCH_STANDARD_API(kroko_transcribe_function)
{
	char *mycmd = NULL, *argv[6] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = 0; /*SMBF_READ_STREAM | SMBF_WRITE_STREAM | SMBF_READ_PING */;

	if (!zstr(cmd) && (mycmd = strdup(cmd))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(cmd) || 
      (!strcasecmp(argv[1], "stop") && argc < 2) ||
      (!strcasecmp(argv[1], "start") && argc < 3) ||
      zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", cmd, argv[0], argv[1]);
		stream->write_function(stream, "-USAGE: %s\n", TRANSCRIBE_API_SYNTAX);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;

		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
				char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing\n");
				status = do_stop(lsession, bugname);
			} else if (!strcasecmp(argv[1], "start")) {
				char* lang = argv[2];
				int interim = argc > 3 && !strcmp(argv[3], "interim");
				char *bugname = argc > 5 ? argv[5] : MY_BUG_NAME;
				if (argc > 4 && !strcmp(argv[4], "stereo")) {
					flags |= SMBF_WRITE_STREAM ;
					flags |= SMBF_STEREO;
				}
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start transcribing %s %s\n", lang, interim ? "interim": "complete");
				status = start_capture(lsession, flags, lang, interim, bugname);
			}
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "+OK Success\n");
	} else {
		stream->write_function(stream, "-ERR Operation Failed\n");
	}

  done:

	switch_safe_free(mycmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_APP(kroko_transcribe_app_function)
{
	char *mycmd = NULL, *argv[6] = { 0 };
	int argc = 0;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_bug_flag_t flags = 0;

	if (!zstr(data) && (mycmd = strdup(data))) {
		argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));
	}

	if (zstr(data) || 
      (!strcasecmp(argv[1], "stop") && argc < 2) ||
      (!strcasecmp(argv[1], "start") && argc < 3) ||
      zstr(argv[0])) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error with command %s %s %s.\n", data, argv[0], argv[1]);
		goto done;
	} else {
		switch_core_session_t *lsession = NULL;
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE,"argv[0], %s ... %s %s",argv[0],argv[1],argv[2]);
		if ((lsession = switch_core_session_locate(argv[0]))) {
			if (!strcasecmp(argv[1], "stop")) {
				char *bugname = argc > 2 ? argv[2] : MY_BUG_NAME;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "stop transcribing\n");
				status = do_stop(lsession, bugname);
			} else if (!strcasecmp(argv[1], "start")) {
				char* lang = argv[2];
				int interim = argc > 3 && !strcmp(argv[3], "interim");
				char *bugname = argc > 5 ? argv[5] : MY_BUG_NAME;
				if (argc > 4 && !strcmp(argv[4], "stereo")) {
					flags |= SMBF_WRITE_STREAM ;
					flags |= SMBF_STEREO;
				}
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "start transcribing %s %s\n", lang, interim ? "interim": "complete");
				status = start_capture(lsession, flags, lang, interim, bugname);
			}
			switch_core_session_rwunlock(lsession);
		}
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,"the 'start_capture()' is finished successful");
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,"the 'start_capture()' isn't finished successful");
	}

  done:

	switch_safe_free(mycmd);
}

SWITCH_MODULE_LOAD_FUNCTION(mod_kroko_transcribe_load)
{
	switch_api_interface_t *api_interface;
	switch_application_interface_t *app_interface;

	/* create/register custom event message type */
	if (switch_event_reserve_subclass(TRANSCRIBE_EVENT_RESULTS) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't register subclass %s!\n", TRANSCRIBE_EVENT_RESULTS);
		return SWITCH_STATUS_TERM;
	}

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	memset(&mod_kroko_transcribe_globals, 0, sizeof(mod_kroko_transcribe_globals_t));
	mod_kroko_transcribe_globals.pool = pool;
	mod_kroko_transcribe_globals.debug = 0;
	switch_core_hash_init(&(mod_kroko_transcribe_globals.profiles));

	if ( mod_kroko_transcribe_load_cfg() != SWITCH_STATUS_SUCCESS ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to load due to bad configs\n");
		return SWITCH_STATUS_TERM;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "kroko Speech Transcription API loading..\n");

  if (SWITCH_STATUS_FALSE == kroko_transcribe_init()) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "Failed initializing dg speech interface\n");
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "kroko Speech Transcription API successfully loaded\n");

	SWITCH_ADD_APP(app_interface, "kroko_transcribe", "kroko Speech Transcription APP", "kroko Transcribe", kroko_transcribe_app_function, TRANSCRIBE_API_SYNTAX, SAF_NONE);
	SWITCH_ADD_API(api_interface, "uuid_kroko_transcribe", "kroko Speech Transcription API", kroko_transcribe_function, TRANSCRIBE_API_SYNTAX);
	switch_console_set_complete("add uuid_kroko_transcribe start lang-code [interim|final] [stereo|mono]");
	switch_console_set_complete("add uuid_kroko_transcribe stop ");

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

/*
  Called when the system shuts down
  Macro expands to: switch_status_t mod_kroko_transcribe_shutdown() */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_kroko_transcribe_shutdown)
{
	kroko_transcribe_cleanup();
	switch_event_free_subclass(TRANSCRIBE_EVENT_RESULTS);
	return SWITCH_STATUS_SUCCESS;
}
