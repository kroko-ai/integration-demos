#ifndef _RES_SPEECH_KROKO_H
#define _RES_SPEECH_KROKO_H

#define KROKO_ENGINE_CONFIG    "res_speech_kroko.conf"
#define KROKO_BUF_SIZE         7680

#define KROKO_RESAMPLE_QUALITY 2
#define KROKO_CHANNELS         1
#define KROKO_SAMPLE_RATE      16000

#define TLS     "wss"
#define noTLS   "ws"

#include <speex/speex_resampler.h>

typedef enum result_mode_s {
    none=0,
    text,
    json
} result_mode_t;

typedef enum channel_mode_s {
  read_mode = 0,
  write_mode,
  rw_mode
} channel_mode_t;

struct kroko_speech_s {
    /* Name of the speech object to be used for logging */
    char                        *name;
    /* Speex resampler */
    SpeexResamplerState         *resampler;
    /* Websocket connection to Kroko ASR */
    struct	ast_websocket       *ws;
    struct  ast_websocket       *ws_w;
    int                         second_ws_using;
    /* Webscoket connection to callback */
    struct	ast_websocket       *cb_ws;
    /* TLS setup */
    struct ast_tls_config       *tls_cfg;
    struct ast_tls_config       *tls_cfg_w;
    /* result mode id */
    result_mode_t               result_mode_id;
    /* channel mode id */
    channel_mode_t              channel_mode_id;
    /* Buffer for frames */
    char                        buf[KROKO_BUF_SIZE];
    char                        buf_w[KROKO_BUF_SIZE];
    int                         offset;
    int                         offset_w;
    char                        *last_result;
    FILE                        *fp;
    const char                  *call_uuid;
    const char                  *src;
    const char                  *dst;
};

struct kroko_engine_s {
    /* Websocket url*/
    char                        *ws_url;
    /* */
    char                        *c_url;
    /* callback url - send the result to it */
    char                        *cb_url;
    /* ws or wss */
    char                        *ws_type;
    /* Kroko API-KEY */
    char                        *api_key;
    /* Language */
    char                        *lang;
    /* sample rate to Kroko */
    unsigned int                sample_rate;
    /* */
    char                        *endpoints;
    /* result mode, none|text|json */
    char                        *result_mode;
    /* channel mode, ro|wo|rw */
    char                        *channel_mode;
    /* Speech engine name */
    char                        *name;
    /* pointer for current speech engine */
    struct ast_speech_engine    *engine;
    /* pointer to next speech engine */
    struct kroko_engine_s         *next;
};

struct kroko_globals_s {
    struct kroko_engine_s *engines;
    unsigned short 	debug;
};

typedef struct kroko_speech_s kroko_speech_t;
typedef struct kroko_engine_s kroko_engine_t;
typedef struct kroko_globals_s kroko_globals_t;

int kroko_engine_config_load(void);
kroko_speech_t *kroko_speech_create(char *engine_name, struct ast_format *format, const char *lang);
int kroko_speech_write(kroko_speech_t *kroko_speech, void *data, int len);
int kroko_speech_destroy(kroko_speech_t *kroko_speech);

unsigned short kroko_speech_debug(void);
#endif