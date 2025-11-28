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
 * === Created by Banafo Ltd, Copyright 2025 ===
 *
 * Requires the 1.2 version of the Speex library
 *
/* Asterisk includes. */
#include "asterisk.h"
#include <asterisk/app.h>
#include <asterisk/module.h>
#include <asterisk/channel.h>
#include <asterisk/audiohook.h>
#include <asterisk/frame.h>
#include <asterisk/file.h>
#include <asterisk/format_cache.h>

#define AST_MODULE "res_audiohook_kroko"
#define AUDIOHOOK_NAME "kroko_hook"

#include <asterisk/http_websocket.h>

#include "../res-speech-kroko/res_speech_kroko.h"

#include "asterisk/datastore.h"


struct kroko_audiohook_data {
    char *engine_name;
    kroko_speech_t *kroko_speech;
    struct ast_audiohook audiohook;

    struct ast_filestream *fs;
    struct ast_filestream *fd;
};

#define KROKO_DATASTORE_TYPE "kroko_audiohook_datastore"

static int kroko_audiohook_stop(struct kroko_audiohook_data *data);

static void kroko_ds_destroy(void *data)
{
    struct kroko_audiohook_data *dt = (struct kroko_audiohook_data *)data;
    if (dt->fs) {
        ast_closestream(dt->fs);
        dt->fs = NULL;
    }

    if (dt->fd) {
        ast_closestream(dt->fd);
        dt->fd = NULL;
    }

    if (dt->kroko_speech != NULL) {
        ast_log(LOG_DEBUG, "destroy kroko Speech");
        kroko_speech_destroy(dt->kroko_speech);
    }

    kroko_audiohook_stop(dt);

    ast_free(data);
    data=NULL;

    ast_log(LOG_DEBUG, "destroy kroko datastore\n");
}

static const struct ast_datastore_info kroko_ds_info = {
    .type = "kroko-audiohook",
    .destroy = kroko_ds_destroy,
};

static struct kroko_audiohook_data *kroko_get_datastore(struct ast_channel *chan) {
    struct ast_datastore *datastore;

    if (!chan) {
        ast_log(LOG_ERROR, "No channel!\n");
        return NULL;
    }

    ast_channel_lock(chan);
    datastore = ast_channel_datastore_find(chan, &kroko_ds_info, KROKO_DATASTORE_TYPE);
    ast_channel_unlock(chan);

    return datastore ? (struct kroko_audiohook_data *)datastore->data : NULL;
}

static void kroko_attach_datastore(struct ast_channel *chan, struct kroko_audiohook_data *data) {
    struct ast_datastore *datastore;

    if (!chan || !data) {
        ast_log(LOG_ERROR, "No channel or data!\n");
        return;
    }

    ast_channel_lock(chan);
    datastore = ast_datastore_alloc(&kroko_ds_info, KROKO_DATASTORE_TYPE);
    if (!datastore) {
        ast_log(LOG_ERROR, "Failed to allocate datastore\n");
        ast_channel_unlock(chan);
        return;
    }

    datastore->data = data;
    ast_channel_datastore_add(chan, datastore);
    ast_channel_unlock(chan);

    ast_log(LOG_DEBUG, "add Kroko datastore\n");
}

static int kroko_audiohook_stop(struct kroko_audiohook_data *data) {
    if (data) {
        ast_audiohook_detach(&data->audiohook);
        ast_audiohook_destroy(&data->audiohook);

        ast_log(LOG_DEBUG, "stop Kroko audiohook\n");

        return 0;
    }

    ast_log(LOG_ERROR, "Kroko audiohook isn't detached!\n");
    return -1;
}

static int kroko_audiohook_callback(struct ast_audiohook *audiohook, struct ast_channel *chan, struct ast_frame *frame, enum ast_audiohook_direction direction) {
    struct kroko_audiohook_data *data;

    /* Hangup !? ... does it have better way to capture this event??? */
    if (frame == NULL) {
        if (kroko_speech_debug()) {
            ast_log(LOG_DEBUG, "HACK after HANGUP (frame=NULL)???\n");
        }
        if (audiohook != NULL) {
            audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
        }
        return 0;
    }

    if (frame->frametype != AST_FRAME_VOICE) {
        return 0;
    }

    if (audiohook->status == AST_AUDIOHOOK_STATUS_DONE) {
        ast_log(LOG_DEBUG, "Kroko audiohook callback is finished\n");
        return 0;
    }

    data = kroko_get_datastore(chan);
    if (data == NULL) {
        ast_log(LOG_ERROR, "ERROR: Can't get kroko datastore!\n");
        return -1;
    } else {
        if (data->kroko_speech == NULL) {
            ast_log(LOG_ERROR, "ERROR: Kroko speech pointer is NULL\n");
            return -1;
        }
    }

    data->kroko_speech->second_ws_using = 0;
    channel_mode_t channel_mode_id = data->kroko_speech->channel_mode_id;
    if (channel_mode_id == rw_mode || channel_mode_id == read_mode) {
        if (direction == AST_AUDIOHOOK_DIRECTION_READ) {
            if (kroko_speech_debug()) {
                if (ast_writestream(data->fs, frame) < 0) {
                    ast_log(LOG_ERROR, "ERROR: ...ast_writestream\n");
                }
            }
            if (kroko_speech_write(data->kroko_speech, frame->data.ptr, frame->datalen) == -1) {
                ast_log(LOG_ERROR, "ERROR: ...kroko_speech_write\n");
                audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
            }
        }
    }

    if (channel_mode_id == rw_mode || channel_mode_id == write_mode) {
        if (direction == AST_AUDIOHOOK_DIRECTION_WRITE) {
            if (kroko_speech_debug()) {
                if (ast_writestream(data->fd, frame) < 0) {
                    ast_log(LOG_ERROR, "ERROR: ...ast_writestream\n");
                }
            }
            if (channel_mode_id == rw_mode) data->kroko_speech->second_ws_using = 1;

            if (kroko_speech_write(data->kroko_speech, frame->data.ptr, frame->datalen) == -1) {
                ast_log(LOG_ERROR, "ERROR: ...kroko_speech_write\n");
                audiohook->status = AST_AUDIOHOOK_STATUS_DONE;
            }
        }
    }

    return 0;
}

static int kroko_audiohook_start(struct ast_channel *chan, const char *engine_name, const char *lang) {
    unsigned int channel_rate;
    struct kroko_audiohook_data *dt;

    const char *src = ast_channel_caller(chan)->id.number.str;
    const char *call_uuid = ast_channel_uniqueid(chan);
    const char *dst = ast_channel_exten(chan);

    dt = ast_calloc(1,sizeof(struct kroko_audiohook_data));
    if (dt == NULL) {
        ast_log(LOG_ERROR, "Failed to allocate memory\n");
        return -1;
    }

    dt->fs = NULL;
    dt->fd = NULL;
    dt->kroko_speech = NULL;
    dt->engine_name = ast_strdup(engine_name);

    if (kroko_speech_debug()) {
        char tmp_filename_r[256];
        sprintf(tmp_filename_r, "/tmp/ast_kroko_test_r_%s_%s_%s", src, dst, call_uuid);
        if (!(dt->fs = ast_writefile(tmp_filename_r, "wav", NULL, O_CREAT | O_WRONLY, 0, AST_FILE_MODE))) {
            ast_log(LOG_ERROR, "Failed to open audio file\n");
            goto ret_error;
        }

        char tmp_filename_w[256];
        sprintf(tmp_filename_w, "/tmp/ast_kroko_test_w_%s_%s_%s", src, dst, call_uuid);
        if (!(dt->fd = ast_writefile(tmp_filename_w, "wav", NULL, O_CREAT | O_WRONLY, 0, AST_FILE_MODE))) {
            ast_log(LOG_ERROR, "Failed to open audio file\n");
            goto ret_error;
        }
    }

    ast_log(LOG_DEBUG, "start Kroko audiohook [%s]\n", dt->engine_name);

    ast_audiohook_init(&dt->audiohook, AST_AUDIOHOOK_TYPE_MANIPULATE, AUDIOHOOK_NAME, AST_AUDIOHOOK_MANIPULATE_ALL_RATES);
    dt->audiohook.manipulate_callback = kroko_audiohook_callback;

    channel_rate = ast_format_get_sample_rate(ast_channel_readformat(chan));
    dt->audiohook.format = ast_format_cache_get_slin_by_rate(channel_rate);

    dt->kroko_speech = kroko_speech_create(dt->engine_name, dt->audiohook.format, lang);
    if ( dt->kroko_speech == NULL) {
        ast_log(LOG_ERROR, "ERROR: The kroko Speech pointer is NULL!\n");
        goto ret_error;
    }

    if (ast_audiohook_attach(chan, &dt->audiohook) != 0) {
        ast_log(LOG_ERROR, "Failed to attach audiohook\n");
        goto ret_error;
    }

    ast_log(LOG_DEBUG, "KrokoAudioHook attached!\n");

    dt->kroko_speech->src = src;
    dt->kroko_speech->call_uuid = call_uuid;
    dt->kroko_speech->dst = dst;

    kroko_attach_datastore(chan, dt);

    return 0;

ret_error:
    ast_audiohook_destroy(&dt->audiohook);
    ast_free(dt);
    dt = NULL;

    /*
        If it has to proceed(doesn't stop the current call), then use 'return 0'!
        'return -1' will stop the call!
    */
    return 0;
}

static int kroko_audiohook_exec(struct ast_channel *chan, const char *data) {
    char *parse;

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(engine_name);
        AST_APP_ARG(lang);
    );

    parse = ast_strdupa(data);
    AST_STANDARD_APP_ARGS(args, parse);

    if (args.engine_name) {
        ast_log(LOG_NOTICE, "KrokoAudioHook 'engine_name' arg: %s\n", args.engine_name);
    } else {
        ast_log(LOG_ERROR, "ERROR: KrokoAudioHook can't be started! The 'engine_name' arg is empty!\n");
        return -1;
    }

    if (args.lang) {
        ast_log(LOG_NOTICE, "KrokoAudioHook 'lang' arg: %s\n", args.lang);
    }

    ast_log(LOG_NOTICE, "exec Kroko audiohook: % s\n", data);
    return kroko_audiohook_start(chan, args.engine_name, args.lang);
}

static int load_module(void) {
    return ast_register_application("KrokoAudioHook", kroko_audiohook_exec, "Starts a Kroko audiohook", NULL);
}

static int unload_module(void) {
    return ast_unregister_application("KrokoAudioHook");
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Kroko AudioHook",
    .support_level = AST_MODULE_SUPPORT_EXTENDED,
    .load = load_module,
    .unload = unload_module,
    .load_pri = AST_MODPRI_CHANNEL_DEPEND,
    .requires = "res_speech_kroko",
);