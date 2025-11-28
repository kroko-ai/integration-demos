/*
 * Asterisk -- An open source telephony toolkit.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 *
 * Please follow coding guidelines 
 * https://docs.asterisk.org/Development/Policies-and-Procedures/Coding-Guidelines/
 * 
 * === Modifications made by Banafo Ltd, Copyright 2024 ===
 * This module contains modifications and original code written by Banafo Ltd.
 * Changes include:
 * - Original configuration for kroko audio format and result format handling.
 * - Custom resampling logic.
 * - Callback implementations for enhanced audio processing.
 *
 * This module also incorporates code from other Asterisk modules, including:
 * - Code based on 'res_speech_vosk' (https://github.com/alphacep/vosk-asterisk).
 * - Concepts and ideas adapted from 'res_speech', 'res_aeap', and 'res_speech_aeap'.
 *
/* Asterisk includes. */
#include "asterisk.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"

#define AST_MODULE "res_speech_kroko"
#include <asterisk/module.h>
#include <asterisk/config.h>
#include "asterisk/utils.h"

#include <asterisk/frame.h>
#include <asterisk/speech.h>
#include <asterisk/format_cache.h>
#include <asterisk/json.h>

#include <asterisk/http_websocket.h>

#include "res_speech_kroko.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

static kroko_globals_t kroko_globals;

static kroko_engine_t *kroko_engine_find_eng(const char *name);
char *kroko_engine_compose_api_wss_url(kroko_engine_t *eng, const char *lang);

unsigned short kroko_speech_debug(void) 
{
    return kroko_globals.debug;
}

struct ast_tls_config *kroko_speech_tls_set(void)
{
	struct ast_tls_config *tls_cfg = ast_calloc(1,sizeof(struct ast_tls_config));

	tls_cfg->enabled = 1;
	tls_cfg->flags.flags |= AST_SSL_DONT_VERIFY_SERVER;
	//tls_cfg->flags.flags |= AST_SSL_IGNORE_COMMON_NAME;
	tls_cfg->flags.flags |= AST_SSL_SERVER_CIPHER_ORDER;

	return tls_cfg;
}

kroko_speech_t *kroko_speech_create(char *engine_name, struct ast_format *format, const char *lang)
{
	int err;
	char *url;
	kroko_engine_t *engine;
	kroko_speech_t *kroko_speech;
	enum ast_websocket_result result, cb_result;

	kroko_speech = ast_calloc(1, sizeof(kroko_speech_t));
	kroko_speech->name = engine_name;

	int f_sample_rate = ast_format_get_sample_rate(format);

	engine = kroko_engine_find_eng(kroko_speech->name);
	if (engine == NULL) {
		ast_log(LOG_ERROR, "The SPEECH engine [%s] wasn't found!\n", kroko_speech->name);
		ast_free(kroko_speech);
		return NULL;
	}

	if (engine->sample_rate == 0) {
		engine->sample_rate = KROKO_SAMPLE_RATE;
	}

	if (engine->c_url != NULL) {
		if (lang) {
			/* override 'c_url' but without to change in the engine struct */
			url = kroko_engine_compose_api_wss_url(engine, lang);
		} else {
			url = engine->c_url;
		}
	} else if (engine->ws_url != NULL) {
		url = engine->ws_url;
	} else {
		ast_log(LOG_ERROR, "It doesn't have URL!\n");
		ast_free(kroko_speech);
		return NULL;
	}

	if (engine->channel_mode != NULL) {
		if (strcmp(engine->channel_mode,"ro") == 0) {
			kroko_speech->channel_mode_id = read_mode;
		} else if (strcmp(engine->channel_mode,"wo") == 0) {
			kroko_speech->channel_mode_id = write_mode;
		} else if (strcmp(engine->channel_mode,"rw") == 0) {
			kroko_speech->channel_mode_id = rw_mode;
		} else {
			kroko_speech->channel_mode_id = read_mode;
		}
		ast_log(LOG_NOTICE, "(%s) Channel mode is '%s' (%d)\n", kroko_speech->name, engine->channel_mode, kroko_speech->channel_mode_id);
	} else {
		ast_log(LOG_ERROR, "(%s) Channel mode doesn't set!\n", kroko_speech->name);
	}

	if (strcmp(engine->ws_type,TLS) == 0) {
		kroko_speech->tls_cfg = kroko_speech_tls_set();

		if (kroko_speech->channel_mode_id == rw_mode) {
			kroko_speech->tls_cfg_w = kroko_speech_tls_set();
		}
	} else if (strcmp(engine->ws_type, noTLS) == 0) {
		kroko_speech->tls_cfg = NULL;
		kroko_speech->tls_cfg_w = NULL;
		ast_log(LOG_DEBUG, "No SSL Setup\n");
	} else {
		ast_log(LOG_ERROR, "ERROR!!!\nUnknown format\nCheck your url in the configuration!\n");
		ast_free(kroko_speech);
		url = engine->c_url;

		return NULL;
	}

	kroko_speech->ws = ast_websocket_client_create(url, engine->ws_type, kroko_speech->tls_cfg, &result);
	if (!kroko_speech->ws) {
		ast_log(LOG_ERROR, "Can't create websocket to '%s'(result=%d)\n", url, result);
		ast_free(kroko_speech);
		return NULL;
	}
	ast_log(LOG_NOTICE, "(%s) Created kroko speech websocket connection (%d)\n", kroko_speech->name, result);

	if (kroko_speech->channel_mode_id == rw_mode) {
		kroko_speech->ws_w = ast_websocket_client_create(url, engine->ws_type, kroko_speech->tls_cfg_w, &result);
		if (!kroko_speech->ws_w) {
			ast_log(LOG_ERROR, "Can't create second websocket to '%s'(result=%d)\n", url, result);
			ast_free(kroko_speech);
			return NULL;
		}
		ast_log(LOG_NOTICE, "(%s) Created kroko speech second websocket connection (%d)\n", kroko_speech->name, result);
	}

	if (f_sample_rate != engine->sample_rate) {
		kroko_speech->resampler = speex_resampler_init(KROKO_CHANNELS, f_sample_rate, engine->sample_rate, KROKO_RESAMPLE_QUALITY, &err);
		if (0 != err) {
			ast_log(LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
			ast_free(kroko_speech);
			return NULL;
		}
		ast_log(LOG_NOTICE, "resampling: %d -> %d", f_sample_rate, engine->sample_rate);
	} else {
		ast_log(LOG_NOTICE, "no resampling\n");
	}

	if (engine->cb_url != NULL) {
		kroko_speech->cb_ws = ast_websocket_client_create(engine->cb_url, "ws", NULL, &cb_result);
		if (!kroko_speech->cb_ws) {
			ast_log(LOG_ERROR, "Can't create websocket to '%s' (callback_url)\n", engine->cb_url);
		}
		ast_log(LOG_NOTICE, "(%s) Created callback websocket connection (%d)\n", kroko_speech->name, cb_result);
	}

	if (engine->result_mode != NULL) {
		if (strcmp(engine->result_mode,"text") == 0) {
			kroko_speech->result_mode_id = text;
		} else if (strcmp(engine->result_mode,"json") == 0) {
			kroko_speech->result_mode_id = json;
		} else {
			kroko_speech->result_mode_id = none;
		}
		ast_log(LOG_NOTICE, "(%s) Result mode is '%s' (%d)\n", kroko_speech->name, engine->result_mode, kroko_speech->result_mode_id);
	} else {
		ast_log(LOG_NOTICE, "(%s) Result mode doesn't set!\n", kroko_speech->name);
	}

	return kroko_speech;
}

struct ast_json *kroko_speech_write_cb(kroko_speech_t *kroko_speech)
{
	struct ast_json* json = ast_json_object_create();

	if (json) {
		/* Call-ID */
		if (kroko_speech->call_uuid != NULL) {
			ast_json_object_set(json, "call_uuid", ast_json_string_create(kroko_speech->call_uuid));
		} else {
			ast_json_object_set(json, "call_uuid", ast_json_string_create(""));
		}

		if (kroko_speech->channel_mode_id == read_mode) {
			/* Caller-ID */
			if (kroko_speech->src != NULL) {
				ast_json_object_set(json, "src", ast_json_string_create(kroko_speech->src));
			} else {
				ast_json_object_set(json, "src", ast_json_string_create(""));
			}

			/* EXTEN , destination */
			if (kroko_speech->dst != NULL) {
				ast_json_object_set(json, "dst", ast_json_string_create(kroko_speech->dst));
			} else {
				ast_json_object_set(json, "dst", ast_json_string_create(""));
			}

			ast_json_object_set(json, "type", ast_json_string_create("local"));
		} else if (kroko_speech->channel_mode_id == write_mode) {
			if (kroko_speech->src != NULL) {
				ast_json_object_set(json, "dst", ast_json_string_create(kroko_speech->src));
			} else {
				ast_json_object_set(json, "dst", ast_json_string_create(""));
			}

			if (kroko_speech->dst != NULL) {
				ast_json_object_set(json, "src", ast_json_string_create(kroko_speech->dst));
			} else {
				ast_json_object_set(json, "src", ast_json_string_create(""));
			}

			ast_json_object_set(json, "type", ast_json_string_create("remote"));
		} else if (kroko_speech->channel_mode_id == rw_mode && kroko_speech->second_ws_using == 0) {
			if (kroko_speech->src != NULL) {
				ast_json_object_set(json, "src", ast_json_string_create(kroko_speech->src));
			} else {
				ast_json_object_set(json, "src", ast_json_string_create(""));
			}

			if (kroko_speech->dst != NULL) {
				ast_json_object_set(json, "dst", ast_json_string_create(kroko_speech->dst));
			} else {
				ast_json_object_set(json, "dst", ast_json_string_create(""));
			}

			ast_json_object_set(json, "type", ast_json_string_create("local"));
		} else if (kroko_speech->channel_mode_id == rw_mode && kroko_speech->second_ws_using == 1) {
			if (kroko_speech->src != NULL) {
				ast_json_object_set(json, "dst", ast_json_string_create(kroko_speech->src));
			} else {
				ast_json_object_set(json, "dst", ast_json_string_create(""));
			}

			if (kroko_speech->dst != NULL) {
				ast_json_object_set(json, "src", ast_json_string_create(kroko_speech->dst));
			} else {
				ast_json_object_set(json, "src", ast_json_string_create(""));
			}

			ast_json_object_set(json, "type", ast_json_string_create("remote"));
		} else {
			ast_json_object_set(json, "src", ast_json_string_create(""));
			ast_json_object_set(json, "dst", ast_json_string_create(""));
			ast_json_object_set(json, "type", ast_json_string_create("undef"));
		}

		ast_json_object_set(json, "timestamp", ast_json_integer_create(time(NULL)));

		/* Final result from kroko - original JSON message */
		if (kroko_speech->last_result != NULL) {
			ast_json_object_set(json, "original_msg", ast_json_string_create(kroko_speech->last_result));
		} else {
			ast_json_object_set(json, "original_msg", ast_json_string_create(""));
		}

		return json;
	}

	return NULL;
}

int kroko_speech_read(kroko_speech_t *kroko_speech, void *data, int datalen)
{
	struct ast_json* json_ptr;

	if (datalen > 0) {
		if (kroko_globals.debug) {
			ast_log(LOG_DEBUG, "(%s) Got result: '%s' [%d]\n", kroko_speech->name, data, datalen);
		}

		if (kroko_speech->last_result != NULL) {
			ast_free(kroko_speech->last_result);
			kroko_speech->last_result = NULL;
		}

		struct ast_json_error err;
		struct ast_json *res_json = ast_json_load_string(data, &err);
		if (res_json != NULL) {
			if (kroko_speech->result_mode_id > 0) {
				const char *json_text = ast_json_object_string_get(res_json, "text");
				const char *json_type = ast_json_object_string_get(res_json, "type");
				if ((json_type != NULL) && (strcmp(json_type,"final")==0) && (strlen(json_text)>0)) {
					ast_log(LOG_DEBUG, "(%s) kroko recognition result: %s\n", kroko_speech->name, json_type);
					if (kroko_speech->result_mode_id == text) {
						if ((json_text != NULL) && (strlen(json_text) > 0)) {
							ast_log(LOG_DEBUG, "(%s) Recognition result: %s\n", kroko_speech->name, json_text);
							kroko_speech->last_result = ast_strdup(json_text);
						}
					} else if (kroko_speech->result_mode_id == json) {
						ast_log(LOG_DEBUG, "(%s) Recognition result: %s\n", kroko_speech->name, data);
						kroko_speech->last_result = ast_strdup(data);
					}
				}
			} else {
				kroko_speech->last_result = ast_strdup(data);
			}

			// send to callback if it's exist as param in the config
			if ((kroko_speech->cb_ws != NULL)&&(kroko_speech->last_result != NULL)) {
				json_ptr = kroko_speech_write_cb(kroko_speech);
				if (json_ptr != NULL) {
					char* json_str = ast_json_dump_string(json_ptr);
					if (json_str) {
						if (kroko_globals.debug) {
							ast_log(LOG_DEBUG, "JSON-CB: %s\n", json_str);
						}
						ast_websocket_write(kroko_speech->cb_ws, AST_WEBSOCKET_OPCODE_TEXT, json_str, strlen(json_str));
						ast_free(json_str);
						if (kroko_speech->last_result != NULL) {
							ast_free(kroko_speech->last_result);
							kroko_speech->last_result = NULL;
						}
					}
					ast_json_unref(json_ptr);
				}
			}
		} else {
			ast_log(LOG_ERROR, "(%s) JSON parse error: %s\n", kroko_speech->name, err.text);
		}
		ast_json_free(res_json);
	} else {
		ast_log(LOG_ERROR, "(%s) Got error result %d\n", kroko_speech->name, datalen);
	}

	return 0;
}

int kroko_speech_wait_reading(kroko_speech_t *kroko_speech) {
	char *res;
	char *buf_read;
	struct sockaddr_in *s;
	char addr_str[16] = {0};
	struct ast_sockaddr *remote_addr;
	uint64_t bytes_read = 0;
	struct ast_websocket *temp_ws;

	if (kroko_speech->second_ws_using == 1) {
		temp_ws = kroko_speech->ws_w;
	} else {
		temp_ws = kroko_speech->ws;
	}

	if (ast_websocket_wait_for_input(temp_ws, 0) > 0) {
		int fragmented = 0;
		enum ast_websocket_opcode opcode;

		res = NULL;
		bytes_read = 0;
		if (ast_websocket_read(temp_ws, &res, &bytes_read, &opcode, &fragmented) != 0) {
			ast_log(LOG_ERROR, "read failure (%d): %s", opcode, strerror(errno));
			return -1;
		}

		remote_addr = ast_websocket_remote_address(temp_ws);
		s = (struct sockaddr_in *)remote_addr;
		inet_ntop(AF_INET, &s->sin_addr, addr_str, sizeof(addr_str));

		switch (opcode) {
			case AST_WEBSOCKET_OPCODE_CONTINUATION:
				ast_log(LOG_DEBUG, "Received CONTINUE from: %s\n", addr_str);
				break;
			case AST_WEBSOCKET_OPCODE_CLOSE:
				ast_log(LOG_DEBUG, "Received CLOSE from: %s\n", addr_str);
				if (temp_ws != NULL) {
					if (ast_websocket_fd(temp_ws) == -1) {
						ast_log(LOG_DEBUG, "the websocket is already closed(1)!\n");
					}
				} else ast_log(LOG_DEBUG, "the websocket is already closed(2)!\n");
				break;
			case AST_WEBSOCKET_OPCODE_BINARY:
				ast_log(LOG_DEBUG, "Received BIN from: %s\n", addr_str);
				break;
			case AST_WEBSOCKET_OPCODE_PING:
				ast_log(LOG_DEBUG, "Received PING from: %s\n", addr_str);
				break;
			case AST_WEBSOCKET_OPCODE_PONG:
				ast_log(LOG_DEBUG, "Received PONG from: %s\n", addr_str);
				break;
			case AST_WEBSOCKET_OPCODE_TEXT:
				buf_read = ast_calloc(1,(bytes_read+1));
				memcpy(buf_read, res, bytes_read);
				kroko_speech_read(kroko_speech, buf_read, bytes_read);
				ast_free(buf_read);
				break;
			default:
				break;
		}
	}

	return 0;
}

int kroko_speech_write(kroko_speech_t *kroko_speech, void *data, int len)
{
	char *res;
	char *buf_read;
	int c,int16_len,fl32_len;

	int16_t _int16[KROKO_BUF_SIZE] = {0};
	float _float32[KROKO_BUF_SIZE] = {0};
	spx_int16_t spx_data[KROKO_BUF_SIZE] = {0};

	char addr_str[16] = {0};
	struct ast_sockaddr *remote_addr;
	struct sockaddr_in *s;

	uint64_t bytes_read = 0;

	int16_len = (len/2);
	fl32_len = (len*2);

	ast_assert (kroko_speech->offset + f32_len < KROKO_BUF_SIZE);
	ast_assert (kroko_speech->offset_w + f32_len < KROKO_BUF_SIZE);

	if (kroko_speech->resampler == NULL) {
		memcpy(_int16 , data, len);

		for(c=0;c < int16_len;c++) {
			_float32[c] = ((float)_int16[c]) / 32768;
		}

		if (kroko_speech->second_ws_using == 0) {
			memcpy(kroko_speech->buf + kroko_speech->offset, _float32, fl32_len);
			kroko_speech->offset += fl32_len;
		} else if (kroko_speech->second_ws_using == 1) {
			memcpy(kroko_speech->buf_w + kroko_speech->offset_w, _float32, fl32_len);
			kroko_speech->offset_w += fl32_len;
		}
	} else {
		float s_float32 = 0;
		spx_uint32_t out_len = KROKO_BUF_SIZE >> 1;

		speex_resampler_process_interleaved_int(kroko_speech->resampler, (const spx_int16_t *) data, (spx_uint32_t *) &int16_len, spx_data, &out_len);
		if (out_len > 0) {
			// bytes written = num samples * 2 * num channels
			size_t bytes_written = out_len << KROKO_CHANNELS;
			char *tmp_ptr;
			if (kroko_speech->second_ws_using == 1) {
				tmp_ptr = kroko_speech->buf_w + kroko_speech->offset_w;
				kroko_speech->offset_w += (bytes_written*2);
			} else {
				tmp_ptr = kroko_speech->buf + kroko_speech->offset;
				kroko_speech->offset += (bytes_written*2);
			}
			for(int i=0;i<out_len;i++) {
				s_float32 = ((float) spx_data[i]) / 32768;
				memcpy(tmp_ptr, &s_float32, sizeof(s_float32));
				tmp_ptr = tmp_ptr + sizeof(s_float32);
			}
		}
	}

	// to first websocket connection
	if (kroko_speech->offset == KROKO_BUF_SIZE) {
		if (ast_websocket_write(kroko_speech->ws, AST_WEBSOCKET_OPCODE_BINARY, kroko_speech->buf, KROKO_BUF_SIZE) == -1){
			ast_log(LOG_ERROR, "ERROR: Can't write data in the WebSocket!\n");
			return -1;
		}
		memset(kroko_speech->buf,0,KROKO_BUF_SIZE);
		kroko_speech->offset = 0;
	}

	// to second websocket connection
	if (kroko_speech->offset_w == KROKO_BUF_SIZE) {
		if (ast_websocket_write(kroko_speech->ws_w, AST_WEBSOCKET_OPCODE_BINARY, kroko_speech->buf_w, KROKO_BUF_SIZE) == -1){
			ast_log(LOG_ERROR, "ERROR: Can't write data in the WebSocket!\n");
			return -1;
		}
		memset(kroko_speech->buf_w,0,KROKO_BUF_SIZE);
		kroko_speech->offset_w = 0;
	}

	kroko_speech_wait_reading(kroko_speech);

	return 0;
}

int kroko_speech_destroy(kroko_speech_t *kroko_speech)
{
	int res_len;
	char *res;

	ast_log(LOG_NOTICE, "(%s) Destroy speech resource\n",kroko_speech->name);

	memset(kroko_speech->buf,0,KROKO_BUF_SIZE);
	memcpy(kroko_speech->buf,"Done", 4);

	if (kroko_speech->ws) {
		ast_websocket_write(kroko_speech->ws, AST_WEBSOCKET_OPCODE_TEXT, kroko_speech->buf, 4);

		if (ast_websocket_wait_for_input(kroko_speech->ws, 2000) > 0) {
			res_len = ast_websocket_read_string(kroko_speech->ws, &res);
			if (res_len >= 0) {
				ast_log(LOG_DEBUG, "WS[%d]: %s\n", res_len, res);
			}
		} else {
			ast_log(LOG_DEBUG, "no wait\n");
		}

		ast_websocket_close(kroko_speech->ws, 1000);
		/* ao2_cleanup(ws) */
		ast_websocket_unref(kroko_speech->ws);
		kroko_speech->ws = NULL;
	}

	if (kroko_speech->ws_w) {
		ast_websocket_write(kroko_speech->ws_w, AST_WEBSOCKET_OPCODE_TEXT, kroko_speech->buf, 4);

		if (ast_websocket_wait_for_input(kroko_speech->ws_w, 2000) > 0) {
			res_len = ast_websocket_read_string(kroko_speech->ws_w, &res);
			if (res_len >= 0) {
				ast_log(LOG_DEBUG, "WS_W[%d]: %s\n", res_len, res);
			}
		} else {
			ast_log(LOG_DEBUG, "no wait\n");
		}

		ast_websocket_close(kroko_speech->ws_w, 1000);
		ast_websocket_unref(kroko_speech->ws_w);

		kroko_speech->ws_w = NULL;
	}

	if (kroko_speech->cb_ws) {
		ast_websocket_close(kroko_speech->cb_ws, 1000);
		ast_websocket_unref(kroko_speech->cb_ws);
		kroko_speech->cb_ws = NULL;
	}

	if (kroko_speech->resampler) {
		speex_resampler_destroy(kroko_speech->resampler);
		kroko_speech->resampler = NULL;
	}

	if (kroko_speech->last_result) {
		ast_free(kroko_speech->last_result);
		kroko_speech->last_result = NULL;
	}
	ast_free(kroko_speech);

	return 0;
}

static int kroko_recog_create(struct ast_speech *speech, struct ast_format *format)
{
	kroko_speech_t *kroko_speech = kroko_speech_create(speech->engine->name, format, NULL);

	if ( kroko_speech == NULL) {
		return -1;
	}

	speech->data = kroko_speech;

	ast_log(LOG_NOTICE, "(%s) Created speech resource result\n", kroko_speech->name);
	return 0;
}

static int kroko_recog_destroy(struct ast_speech *speech)
{
	kroko_speech_t *kroko_speech = speech->data;
	ast_log(LOG_NOTICE, "(%s) Destroy speech resource\n",kroko_speech->name);

	return kroko_speech_destroy(kroko_speech);
}

static int kroko_recog_load_grammar(struct ast_speech *speech, const char *grammar_name, const char *grammar_path)
{
	return 0;
}

static int kroko_recog_unload_grammar(struct ast_speech *speech, const char *grammar_name)
{
	return 0;
}

static int kroko_recog_activate_grammar(struct ast_speech *speech, const char *grammar_name)
{
	return 0;
}

static int kroko_recog_deactivate_grammar(struct ast_speech *speech, const char *grammar_name)
{
	return 0;
}

static int kroko_recog_write(struct ast_speech *speech, void *data, int len)
{
	return kroko_speech_write(speech->data, data, len);
}

static int kroko_recog_dtmf(struct ast_speech *speech, const char *dtmf)
{
	kroko_speech_t *kroko_speech = speech->data;
	ast_log(LOG_NOTICE, "(%s) Signal DTMF %s\n",kroko_speech->name,dtmf);
	return 0;
}

static int kroko_recog_start(struct ast_speech *speech)
{
	kroko_speech_t *kroko_speech = speech->data;
	ast_log(LOG_NOTICE, "(%s) Start recognition\n",kroko_speech->name);
	ast_speech_change_state(speech, AST_SPEECH_STATE_READY);
	return 0;
}

static int kroko_recog_change(struct ast_speech *speech, const char *name, const char *value)
{
	kroko_speech_t *kroko_speech = speech->data;
	ast_log(LOG_NOTICE, "(%s) Change setting name: %s value:%s\n",kroko_speech->name,name,value);
	return 0;
}

static int kroko_recog_get_settings(struct ast_speech *speech, const char *name, char *buf, size_t len)
{
	kroko_speech_t *kroko_speech = speech->data;
	ast_log(LOG_NOTICE, "(%s) Get settings name: %s\n",kroko_speech->name,name);
	return -1;
}

static int kroko_recog_change_results_type(struct ast_speech *speech,enum ast_speech_results_type results_type)
{
	return -1;
}

struct ast_speech_result* kroko_recog_get(struct ast_speech *speech)
{
	struct ast_speech_result *speech_result;

	kroko_speech_t *kroko_speech = speech->data;
	speech_result = ast_calloc(sizeof(struct ast_speech_result), 1);
	speech_result->text = ast_strdup(kroko_speech->last_result);
	speech_result->score = 100;

	ast_set_flag(speech, AST_SPEECH_HAVE_RESULTS);
	return speech_result;
}

static kroko_engine_t *kroko_engine_find_eng(const char *name)
{
	kroko_engine_t *tmp_eng = kroko_globals.engines;
	while ( tmp_eng != NULL) {
		if (strcmp(tmp_eng->name,name) == 0) {
			return tmp_eng;
		}
		tmp_eng = tmp_eng->next;
	}

	return NULL;
}

char *kroko_engine_compose_api_wss_url(kroko_engine_t *eng, const char *lang)
{
	size_t url_len = 0;
	char *composed_url = NULL;

	if (eng->endpoints == NULL) {
		eng->endpoints = ast_strdup("true");
	}

	if (lang) {
		/* All lenghts of variables + 34 bytes ('?','=','=',api param,lang param + endpoints param+ 1 byte for string ending '\0') */
		url_len = strlen(eng->ws_url) + strlen(eng->api_key) + strlen(lang) + strlen(eng->endpoints) + 34 ;

		composed_url = ast_calloc(1,url_len);

		if (composed_url != NULL) {
			sprintf(composed_url,"%s?apiKey=%s&languageCode=%s&endpoints=%s",
					eng->ws_url, eng->api_key, lang, eng->endpoints);
		}
	} else {
		/* All lenghts of variables + 34 bytes ('?','=','=',api param,lang param + endpoints param+ 1 byte for string ending '\0') */
		url_len = strlen(eng->ws_url) + strlen(eng->api_key) + strlen(eng->lang) + strlen(eng->endpoints) + 34 ;

		composed_url = ast_calloc(1,url_len);

		if (composed_url != NULL) {
			sprintf(composed_url,"%s?apiKey=%s&languageCode=%s&endpoints=%s",
					eng->ws_url, eng->api_key, eng->lang, eng->endpoints);
		}
	}

	return composed_url;
}

char *kroko_engine_separate_url(char *url)
{
	char *buf = ast_strdup(url);

	if (buf != NULL) {
		return ast_strsep(&buf, ':', 0);
	}

	return NULL;
}

void kroko_engine_destroy(kroko_engine_t *eng)
{
	if (eng != NULL) {
		if (eng->name != NULL) ast_free(eng->name);
		if (eng->ws_url != NULL) ast_free(eng->ws_url);
		if (eng->c_url != NULL) ast_free(eng->c_url);
		if (eng->engine != NULL) ast_free(eng->engine);
		if (eng->ws_type != NULL) ast_free(eng->ws_type);
		if (eng->api_key != NULL) ast_free(eng->api_key);
		if (eng->lang != NULL) ast_free(eng->lang);
		if (eng->endpoints != NULL) ast_free(eng->endpoints);
		if (eng->cb_url != NULL) ast_free(eng->cb_url);
		if (eng->result_mode != NULL) ast_free(eng->result_mode);
		if (eng->channel_mode != NULL) ast_free(eng->channel_mode);
	}
}

int kroko_engine_config_load()
{
	kroko_engine_t *tmp_eng = NULL,*tmp = NULL;
	const char *value = NULL,*var = NULL, *cat = NULL;
	struct ast_flags config_flags = { 0 };
	struct ast_config *cfg = ast_config_load(KROKO_ENGINE_CONFIG, config_flags);
	if(!cfg) {
		ast_log(LOG_WARNING, "No such configuration file %s\n", KROKO_ENGINE_CONFIG);
		return -1;
	}

	kroko_globals.engines = NULL;
	kroko_globals.debug = 0;

	if((value = ast_variable_retrieve(cfg, "general", "debug")) != NULL) {
		ast_log(LOG_NOTICE, "general.debug=%s\n", value);
		if(strcmp(value,"yes")==0) { 
			kroko_globals.debug = 1;
		}
	}

	cat = ast_category_browse(cfg, "general");
	while (cat != NULL) {
		tmp_eng = ast_calloc(1,sizeof(kroko_engine_t));

		if((var = ast_variable_retrieve(cfg, cat, "url")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.url=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->ws_url = ast_strdup(var);
			} else {
				ast_log(LOG_ERROR, "Empty URL in SPEECH engine [%s]!!!\n", cat);
				kroko_engine_destroy(tmp_eng);
				goto _next;
			}

			tmp_eng->ws_type = kroko_engine_separate_url(tmp_eng->ws_url);
			if (tmp_eng->ws_type != NULL) {
				if ((strcmp(tmp_eng->ws_type,TLS) != 0) && (strcmp(tmp_eng->ws_type,noTLS) != 0)) {
					ast_log(LOG_ERROR, "Unknown format (support ws:// or wss://), your url is : %s !\n", tmp_eng->ws_url);
					kroko_engine_destroy(tmp_eng);
					goto _next;
				}
			} else {
				ast_log(LOG_ERROR, "Can't recognize the link type (ws:// or wss://)!!!\n");
				kroko_engine_destroy(tmp_eng);
				goto _next;
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "callback_url")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.cb_url=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->cb_url = ast_strdup(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "apiKey")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.apiKey=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->api_key = ast_strdup(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "sample_rate")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.sample_rate=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->sample_rate = atoi(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "lang")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.lang=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->lang = ast_strdup(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "endpoints")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.endpoints=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->endpoints = ast_strdup(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "result_mode")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.result_mode=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->result_mode = ast_strdup(var);
			}
		}

		if((var = ast_variable_retrieve(cfg, cat, "channel_mode")) != NULL) {
			if (kroko_globals.debug) ast_log(LOG_DEBUG, "%s.channel_mode=%s\n", cat, var);
			if (strlen(var) > 0) {
				tmp_eng->channel_mode = ast_strdup(var);
			}
		}

		if ((tmp_eng->ws_url != NULL)&&(tmp_eng->api_key != NULL)&&(tmp_eng->lang != NULL)) {
			tmp_eng->c_url = kroko_engine_compose_api_wss_url(tmp_eng, NULL);
			if (tmp_eng->c_url != NULL) {
				ast_log(LOG_NOTICE, "c_url = %s\n", tmp_eng->c_url);
			}
		}

		if (tmp_eng->ws_url == NULL) {
			ast_log(LOG_ERROR, "The SPEECH engine [%s] doesn't have a URL!\nIt didn't add to the SPEECH engine list!\n", cat);
			kroko_engine_destroy(tmp_eng);
			goto _next;
		}

		tmp_eng->name = ast_strdup(cat);

		if (kroko_globals.engines == NULL) {
			kroko_globals.engines = tmp_eng;
			tmp = tmp_eng;
		} else {
			tmp->next = tmp_eng;
			tmp = tmp->next;
		}

_next:
		cat = ast_category_browse(cfg, cat);
	}

	ast_config_destroy(cfg);
	return 0;
}

static struct ast_speech_engine *kroko_recog_speech_engine_alloc(const char *name)
{
	struct ast_speech_engine *engine;

	engine = ast_calloc(1,sizeof(struct ast_speech_engine));
	if (engine == NULL) {
		return  NULL;
	}

	engine->name = ast_strdup(name);
	engine->create = kroko_recog_create;
	engine->destroy = kroko_recog_destroy;
	engine->write = kroko_recog_write;
	engine->dtmf = kroko_recog_dtmf;
	engine->start = kroko_recog_start;
	engine->change = kroko_recog_change;
	engine->get_setting = kroko_recog_get_settings;
	engine->change_results_type = kroko_recog_change_results_type;
	engine->get = kroko_recog_get;

	engine->formats = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);
	if(!engine->formats) {
		ast_log(LOG_ERROR, "Failed to alloc media format capabilities\n");
		ast_free(engine);
		return NULL;
	}

	/*	ast_format_slin is SLIN/8000, 
		ast_format_slin16 is SLIN/16000 
		- second is by default for kroko ! */
	ast_format_cap_append(engine->formats, ast_format_slin16, 0);

	return engine;
}

static int kroko_recog_load_engine(kroko_engine_t *kroko_engine_ptr)
{
	if (kroko_engine_ptr == NULL) {
		ast_log(LOG_ERROR, "Don't have kroko configuration!!!\n");
		return -1;
	}

	ast_log(LOG_NOTICE, "load kroko engine: %s\n", kroko_engine_ptr->name);
	if (kroko_engine_ptr->name == NULL) {
		ast_log(LOG_ERROR, "didn't recognize kroko engine name ('[]',it's empty in the config file)!\n");
		return -1;
	}

	kroko_engine_ptr->engine = kroko_recog_speech_engine_alloc(kroko_engine_ptr->name);
	if ( kroko_engine_ptr->engine == NULL) {
		ast_log(LOG_ERROR, "Failed to allocate kroko engine mem: %s\n", kroko_engine_ptr->name);
		return -1;
	}

	if(ast_speech_register(kroko_engine_ptr->engine)) {
		ast_log(LOG_ERROR, "Failed to register kroko engine: %s\n", kroko_engine_ptr->name);
		return -1;
	}

	return 0;
}

static int load_module(void)
{
	kroko_engine_t *tmp_eng;
	struct ast_speech_engine *engine;

	ast_log(LOG_NOTICE, "Load res_speech_kroko module\n");

	/* Load engine configuration */
	kroko_engine_config_load();

	tmp_eng = kroko_globals.engines;
	if (tmp_eng == NULL) {
		ast_log(LOG_ERROR, "Can't load kroko module\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	while (tmp_eng != NULL) {
		engine = ast_speech_find_engine(tmp_eng->name);
		if (engine != NULL) {
			ast_log(LOG_DEBUG, "re-load engine: %s\n", tmp_eng->name);
			ast_speech_unregister2(engine->name);
		}
		kroko_recog_load_engine(tmp_eng);
		ast_log(LOG_DEBUG, "load engine: %s\n", tmp_eng->name);
		tmp_eng = tmp_eng->next;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

/** \brief Unload module */
static int unload_module(void)
{
	kroko_engine_t *tmp_eng,*tmp;

	tmp_eng = kroko_globals.engines;
	while (tmp_eng != NULL) {
		tmp = tmp_eng;
		ast_log(LOG_DEBUG, "unregister: '%s'\n", tmp->name);
		if (ast_speech_unregister(tmp->name)) {
			ast_log(LOG_ERROR, "Failed to unregister '%s'\n", tmp->name);
		}
		tmp_eng = tmp_eng->next;
		kroko_engine_destroy(tmp);
		tmp->next = NULL;
		ast_free(tmp);
	}

	kroko_globals.engines = NULL;

	ast_log(LOG_NOTICE, "Unload res_speech_kroko module\n");

	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS | AST_MODFLAG_LOAD_ORDER, "kroko Speech Engine",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_speech,res_http_websocket",
);
