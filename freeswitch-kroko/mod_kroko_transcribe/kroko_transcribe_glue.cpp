#include <switch.h>
#include <switch_json.h>
#include <string.h>
#include <string>
#include <mutex>
#include <thread>
#include <list>
#include <algorithm>
#include <functional>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <regex>
#include <iostream>
#include <unordered_map>

#include "mod_kroko_transcribe.h"
#include "simple_buffer.h"
#include "parser.hpp"
#include "audio_pipe.hpp"

#define RTP_PACKETIZATION_PERIOD 20
#define FRAME_SIZE_8000  320 /*which means each 20ms frame as 320 bytes at 8 khz (1 channel only)*/
#define KROKO_API_PATH "/api/v1/transcripts/streaming?"

namespace {
  static bool hasDefaultCredentials = false;
  //static const char* defaultApiKey = nullptr;
  static const char *requestedBufferSecs = std::getenv("MOD_AUDIO_FORK_BUFFER_SECS");
  static int nAudioBufferSecs = std::max(1, std::min(requestedBufferSecs ? ::atoi(requestedBufferSecs) : 2, 5));
  static const char *requestedNumServiceThreads = std::getenv("MOD_AUDIO_FORK_SERVICE_THREADS");
  static unsigned int nServiceThreads = std::max(1, std::min(requestedNumServiceThreads ? ::atoi(requestedNumServiceThreads) : 1, 5));
  static unsigned int idxCallCount = 0;
  static uint32_t playCount = 0;

  static const char* emptyTranscript = "{\"text\": \"\", \"type\": \"";

  static void reaping_helper(kroko::AudioPipe *tmpPtr) {
    std::shared_ptr<kroko::AudioPipe> pAp;
    pAp.reset(tmpPtr);

    if (pAp) {
      std::thread t([pAp]{
        pAp->finish();
        pAp->waitForClose();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "got remote close\n");
      });
      t.detach();
    }
  }

  static void reaper(private_t *tech_pvt) {
    if (!tech_pvt) return;

    if (tech_pvt->pAudioPipeR) {
      reaping_helper((kroko::AudioPipe *)tech_pvt->pAudioPipeR);
      tech_pvt->pAudioPipeR = nullptr;
    }

    if (tech_pvt->pAudioPipeW) {
      reaping_helper((kroko::AudioPipe *)tech_pvt->pAudioPipeW);
      tech_pvt->pAudioPipeW = nullptr;
    }

    if (tech_pvt->pCallBack) {
      reaping_helper((kroko::AudioPipe *)tech_pvt->pCallBack);
      tech_pvt->pCallBack = nullptr;
    }
  }

  static void destroy_tech_pvt(private_t *tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s (%u) destroy_tech_pvt\n", tech_pvt->sessionId, tech_pvt->id);
    if (tech_pvt) {
      if (tech_pvt->pAudioPipeR) {
        kroko::AudioPipe* p = (kroko::AudioPipe *) tech_pvt->pAudioPipeR;
        delete p;
        tech_pvt->pAudioPipeR = nullptr;
      }

      if (tech_pvt->pAudioPipeW) {
        kroko::AudioPipe* p = (kroko::AudioPipe *) tech_pvt->pAudioPipeW;
        delete p;
        tech_pvt->pAudioPipeW = nullptr;
      }

      if (tech_pvt->pCallBack) {
        kroko::AudioPipe* cb = (kroko::AudioPipe *) tech_pvt->pCallBack;
        delete cb;
        tech_pvt->pCallBack = nullptr;
      }

      if (tech_pvt->resampler) {
          speex_resampler_destroy(tech_pvt->resampler);
          tech_pvt->resampler = NULL;
      }

      /*if (tech_pvt->call_info_ptr) {
        free(tech_pvt->call_info_ptr);
        tech_pvt->call_info_ptr = NULL;
      }*/
      /*
      if (tech_pvt->vad) {
        switch_vad_destroy(&tech_pvt->vad);
        tech_pvt->vad = nullptr;
      }
      */
    }
  }

  std::string encodeURIComponent(std::string decoded)
  {

      std::ostringstream oss;
      std::regex r("[!'\\(\\)*-.0-9A-Za-z_~:]");

      for (char &c : decoded)
      {
          if (std::regex_match((std::string){c}, r))
          {
              oss << c;
          }
          else
          {
              oss << "%" << std::uppercase << std::hex << (0xff & c);
          }
      }
      return oss.str();
  }

  std::string& constructPath(switch_core_session_t* session, std::string& path, 
    int sampleRate, int channels, const char* language, int interim) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    const char *var ;
    std::ostringstream oss;

    if (var = switch_channel_get_variable(channel, "KROKO_API_KEY")) {
      oss << KROKO_API_PATH <<  "apiKey=";
      oss << var;
    } else {
      oss << "";
      path = oss.str();
      return path;
    }

    oss <<  "&languageCode=";
    oss <<  language;

    if (var = switch_channel_get_variable(channel, "KROKO_SPEECH_ENDPOINTS")) {
      oss <<  "&endpoints=";
      oss <<  var;
    }

   path = oss.str();
   return path;
  }

  static void eventCallback(const char* sessionId, kroko::AudioPipe::NotifyEvent_t event, const char* message, bool finished,kroko::AudioPipe::EventCallbackType_t ecb_type) {
    switch_core_session_t* session = switch_core_session_locate(sessionId);
    if (session) {
      switch_channel_t *channel = switch_core_session_get_channel(session);
      switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
      if (bug) {
        private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
        if (tech_pvt) {
          call_info_t *call_info_ptr = tech_pvt->call_info_ptr;
          switch (event) {
            case kroko::AudioPipe::CONNECT_SUCCESS:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "connection successful\n");
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_SUCCESS, NULL, tech_pvt->bugname, finished);
            break;
            case kroko::AudioPipe::CONNECT_FAIL:
            {
              // first thing: we can no longer access the AudioPipe
              std::stringstream json;
              json << "{\"reason\":\"" << message << "\"}";
              if (ecb_type == kroko::AudioPipe::READ_EVENT_CB) tech_pvt->pAudioPipeR = nullptr;
              if (ecb_type == kroko::AudioPipe::WRITE_EVENT_CB) tech_pvt->pAudioPipeW = nullptr;
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_CONNECT_FAIL, (char *) json.str().c_str(), tech_pvt->bugname, finished);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "connection failed: %s\n", message);
            }
            break;
            case kroko::AudioPipe::CONNECTION_DROPPED:
              // first thing: we can no longer access the AudioPipe
              if (ecb_type == kroko::AudioPipe::READ_EVENT_CB) tech_pvt->pAudioPipeR = nullptr;
              if (ecb_type == kroko::AudioPipe::WRITE_EVENT_CB) tech_pvt->pAudioPipeW = nullptr;
              tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_DISCONNECT, NULL, tech_pvt->bugname, finished);
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection dropped from far end\n");
            break;
            case kroko::AudioPipe::CONNECTION_CLOSED_GRACEFULLY:
              // first thing: we can no longer access the AudioPipe
              if (ecb_type == kroko::AudioPipe::READ_EVENT_CB) tech_pvt->pAudioPipeR = nullptr;
              if (ecb_type == kroko::AudioPipe::WRITE_EVENT_CB) tech_pvt->pAudioPipeW = nullptr;
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection closed gracefully\n");
            break;
            case kroko::AudioPipe::MESSAGE:
            {
              //if (strcmp(message, "Done!") == 0) {
              //  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "received 'Done!' by kroko ASR server\n");
              //}

              if( strstr(message, emptyTranscript)) {
                if(mod_kroko_transcribe_globals.debug) switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "empty kroko transcript\n");
              }
              else {
                //if(mod_kroko_transcribe_globals.debug) {
                //  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
                //                  "kroko message (full) from the kroko ASR server: %s\n", message);
                //}

                char* final_result = NULL;
                if (tech_pvt->kroko_conn->result_mode > 0) {
                  cJSON* json_result = cJSON_Parse(message);
                  if (json_result != NULL) {
                    const char* plain_type;
                    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json_result, "type");
                    if (cJSON_IsString(type_item) && (type_item->valuestring != NULL)) {
                      plain_type = type_item->valuestring;
                    }

                    cJSON *text_item = cJSON_GetObjectItemCaseSensitive(json_result, "text");
                    if (cJSON_IsString(text_item) && (text_item->valuestring != NULL)) {
                      if (strcmp(plain_type, "final") == 0) {
                        if (tech_pvt->kroko_conn->result_mode == text) {
                          final_result = strdup(text_item->valuestring);
                        } else if (tech_pvt->kroko_conn->result_mode == json) {
                          final_result = strdup(message);
                        }
                      }
                    }
                    cJSON_Delete(json_result);
                  }
                } else {
                  final_result = strdup(message);
                }

                if (final_result) {
                  tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_RESULTS, final_result, tech_pvt->bugname, finished);
                  if(mod_kroko_transcribe_globals.debug) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                    "message from the KROKO ASR: %s\n", final_result);
                  }
                }

                kroko::AudioPipe *pCallBack = static_cast<kroko::AudioPipe *>(tech_pvt->pCallBack);

                if (pCallBack && (final_result != NULL)) {
                  // send transcriptions to the kroko proxy (callback_url)
                  std::string msg = final_result;

                  cJSON* root = cJSON_CreateObject();
                  cJSON_AddStringToObject(root, "call_uuid", sessionId);

                  if (ecb_type == kroko::AudioPipe::READ_EVENT_CB) {
                    cJSON_AddStringToObject(root, "src", call_info_ptr->src);
                    cJSON_AddStringToObject(root, "dst", call_info_ptr->dst);
                    cJSON_AddStringToObject(root, "type", "local");
                  }

                  if (ecb_type == kroko::AudioPipe::WRITE_EVENT_CB) {
                    cJSON_AddStringToObject(root, "src", call_info_ptr->dst);
                    cJSON_AddStringToObject(root, "dst", call_info_ptr->src);
                    cJSON_AddStringToObject(root, "type", "remote");
                  }

                  cJSON_AddNumberToObject(root, "timestamp", time(nullptr));
                  cJSON_AddStringToObject(root, "original_msg", msg.c_str());

                  char* json_str = cJSON_PrintUnformatted(root);
                  cJSON_Delete(root);

                  if(mod_kroko_transcribe_globals.debug) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "[JSON-CB], %s\n", json_str);
                  }
                  pCallBack->listForSending(json_str);
                  free(json_str);
                }

                if (final_result != NULL) free(final_result);
              }
            }
            break;
            default:
              switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_NOTICE, "got unexpected msg from kroko %d:%s\n", event, message);
              break;
          }
        }
      }
      switch_core_session_rwunlock(session);
    }
  }

  int cb_url_parse(cb_url_t *cb_ptr, char *cb_url) {
    char tmp[CB_URL_LEN];
    char *token = NULL, *host = NULL, *port = NULL, *path = NULL;
    strcpy(tmp, cb_url);

    token = strtok(tmp, CB_URL_DEL1);
    if (token != NULL) {
      if (strcmp(token, "ws:") == 0) {
        cb_ptr->cb_prot = ws;
      } else if (strcmp(token, "wss:") == 0) {
        cb_ptr->cb_prot = wss;
      }
    } else {
      return -1;
    }

    token = strtok(NULL, CB_URL_DEL1);
    if (token != NULL) {
      path = strtok(NULL, CB_URL_DEL1);
      if (path != NULL) {
        strcpy(cb_ptr->cb_path, path);
      }

      host = strtok(token, CB_URL_DEL2);
      if (host != NULL) {
        strcpy(cb_ptr->cb_host, host);
      }

      port = strtok(NULL, ":");
      if (port != NULL) {
        cb_ptr->cb_port = atoi(port);
      }
    } else {
      return -1;
    }

    return 0;
  }

  switch_status_t fork_data_init(private_t *tech_pvt, switch_core_session_t *session, 
    int sampling, int channels, char *lang, int interim, 
    char* bugname, responseHandler_t responseHandler) {

    int err;
    int desiredSampling;
    void *vptr = NULL;
    const char *var ;
    mod_kroko_transcribe_conn_t *ptr = NULL;
    switch_codec_implementation_t read_impl;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_core_session_get_read_impl(session, &read_impl);

    if (mod_kroko_transcribe_globals.profiles != NULL) {
      vptr = switch_core_hash_find(mod_kroko_transcribe_globals.profiles,lang);
      if (vptr != NULL) {
        ptr = (mod_kroko_transcribe_conn_t *)vptr;
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "host: %s, port: %d, prot: %d, sample_rate: %d, callback_url: %s;\n",
                          ptr->kroko_asr_hostname, ptr->kroko_asr_port, ptr->prot, ptr->sample_rate,ptr->callback_url);
      } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"the profile is not found!");
        return SWITCH_STATUS_FALSE;
      }
    } else {
      switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,"profiles pointer is NULL!");
      return SWITCH_STATUS_FALSE;
    }

    desiredSampling = ptr->sample_rate;
    memset(tech_pvt, 0, sizeof(private_t));
    tech_pvt->kroko_conn = ptr;

    std::string path;
    constructPath(session, path, desiredSampling, channels, lang, interim);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "path[%d]: %s \n", path.size(), path.c_str());

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID);
    strncpy(tech_pvt->host, ptr->kroko_asr_hostname, MAX_WS_URL_LEN);
    tech_pvt->port = ptr->kroko_asr_port;
    strncpy(tech_pvt->path, path.c_str(), MAX_PATH_LEN);
    tech_pvt->sampling = desiredSampling;
    tech_pvt->responseHandler = responseHandler;
    tech_pvt->channels = channels;
    tech_pvt->id = ++idxCallCount;
    tech_pvt->buffer_overrun_notified = 0;

    /* is it correct for all codec variants ???? */
    size_t buflen = LWS_PRE + (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * 1000 / RTP_PACKETIZATION_PERIOD * nAudioBufferSecs);

    call_info_t* call_info_ptr = (call_info_t *) switch_core_session_alloc(session, sizeof(call_info_t));
    //call_info_t* call_info_ptr = (call_info_t *) malloc(sizeof(call_info_t));
    if (!call_info_ptr) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    if (var = switch_channel_get_variable(channel, "sip_from_user")) {
      strncpy(call_info_ptr->src, var, sizeof(var));
    }

    if (var = switch_channel_get_variable(channel, "sip_to_user")) {
      strncpy(call_info_ptr->dst, var, sizeof(var));
    }

    tech_pvt->call_info_ptr = call_info_ptr;

    const char* apiKey = "";

    if ((ptr->channel_mode == read_mode)||(ptr->channel_mode == rw_mode)) {
      kroko::AudioPipe* ap_read = new kroko::AudioPipe(tech_pvt->sessionId, tech_pvt->host, tech_pvt->port, ptr->prot,tech_pvt->path,
          buflen, read_impl.decoded_bytes_per_packet, apiKey, eventCallback, kroko::AudioPipe::READ_EVENT_CB);
      if (!ap_read) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
        return SWITCH_STATUS_FALSE;
      }

      tech_pvt->pAudioPipeR = static_cast<void *>(ap_read);
    } else {
      tech_pvt->pAudioPipeR = nullptr;
    }

    if ((ptr->channel_mode == write_mode)||(ptr->channel_mode == rw_mode)) {
      kroko::AudioPipe* ap_write = new kroko::AudioPipe(tech_pvt->sessionId, tech_pvt->host, tech_pvt->port, ptr->prot,tech_pvt->path,
          buflen, read_impl.decoded_bytes_per_packet, apiKey, eventCallback, kroko::AudioPipe::WRITE_EVENT_CB);
      if (!ap_write) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe\n");
        return SWITCH_STATUS_FALSE;
      }

      tech_pvt->pAudioPipeW = static_cast<void *>(ap_write);
    } else {
      tech_pvt->pAudioPipeW = nullptr;
    }

    switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

    if (tech_pvt->kroko_conn->callback_url != NULL) {
      cb_url_t cb_url = {0};
      if ( cb_url_parse(&cb_url, tech_pvt->kroko_conn->callback_url) == 0) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
                          "cb_host: %s, cb_port: %d, cb_prot: %d, cb_path: %s;\n",
                          cb_url.cb_host, cb_url.cb_port, cb_url.cb_prot, cb_url.cb_path);

        kroko::AudioPipe* cb = new kroko::AudioPipe(tech_pvt->sessionId, cb_url.cb_host, cb_url.cb_port, cb_url.cb_prot, cb_url.cb_path,
            buflen, read_impl.decoded_bytes_per_packet, apiKey, eventCallback, kroko::AudioPipe::EXT_EVENT_CB);
        if (!cb) {
          switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error allocating AudioPipe (CallBack!)\n");
          return SWITCH_STATUS_FALSE;
        }

        tech_pvt->pCallBack = static_cast<void *>(cb);
      }
    }

    /*
      'desiredSampling' is from 'profile.sample_rate'(value from config)
      'sampling' is from 'codec.samples_per_seconds'
    */
    if (desiredSampling != sampling) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) resampling from %u to %u\n", tech_pvt->id, sampling, desiredSampling);
      tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
      if (0 != err) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
        return SWITCH_STATUS_FALSE;
      }
    }
    else {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) no resampling needed for this call\n", tech_pvt->id);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) fork_data_init\n", tech_pvt->id);

    return SWITCH_STATUS_SUCCESS;
  }

  void lws_logger(int level, const char *line) {
    switch_log_level_t llevel = SWITCH_LOG_DEBUG;

    switch (level) {
      case LLL_ERR: llevel = SWITCH_LOG_ERROR; break;
      case LLL_WARN: llevel = SWITCH_LOG_WARNING; break;
      case LLL_NOTICE: llevel = SWITCH_LOG_NOTICE; break;
      case LLL_INFO: llevel = SWITCH_LOG_INFO; break;
      break;
    }
	  switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "%s\n", line);
  }
}


extern "C" {
  switch_status_t kroko_transcribe_init() {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_kroko_transcribe: audio buffer (in secs):    %d secs\n", nAudioBufferSecs);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "mod_kroko_transcribe: lws service threads:       %d\n", nServiceThreads);
 
    int logs = LLL_ERR | LLL_WARN | LLL_NOTICE || LLL_INFO | LLL_PARSER | LLL_HEADER | LLL_EXT | LLL_CLIENT  | LLL_LATENCY | LLL_DEBUG ;
    
    kroko::AudioPipe::initialize(nServiceThreads, logs, lws_logger);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "AudioPipe::initialize completed\n");

		return SWITCH_STATUS_SUCCESS;
  }

  switch_status_t kroko_transcribe_cleanup() {
    bool cleanup = false;
    cleanup = kroko::AudioPipe::deinitialize();
    if (cleanup == true) {
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
  }
	
  switch_status_t kroko_transcribe_session_init(switch_core_session_t *session, 
    responseHandler_t responseHandler, uint32_t channels, 
    char* lang, int interim, char* bugname, void **ppUserData)
  {
    int err;
    uint32_t samples_per_second;
    switch_codec_implementation_t read_impl;
    memset(&read_impl,0,sizeof(switch_codec_implementation_t));
    switch_core_session_get_read_impl(session, &read_impl);

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, 
                    "codec: %s, samples_per_second: %d, samples_per_packet: %d, decoded_bytes_per_packet: %d\n", 
                    read_impl.iananame, read_impl.samples_per_second, read_impl.samples_per_packet, read_impl.decoded_bytes_per_packet);

    samples_per_second = !strcasecmp(read_impl.iananame, "g722") ? read_impl.actual_samples_per_second : read_impl.samples_per_second;

    // allocate per-session data structure
    private_t* tech_pvt = (private_t *) switch_core_session_alloc(session, sizeof(private_t));
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    call_info_t* call_info_ptr = (call_info_t *) switch_core_session_alloc(session, sizeof(call_info_t));
    if (!call_info_ptr) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
      return SWITCH_STATUS_FALSE;
    }

    tech_pvt->call_info_ptr = call_info_ptr;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "channels: %d\n",channels);
    if (SWITCH_STATUS_SUCCESS != fork_data_init(tech_pvt, session, samples_per_second, channels, lang, interim, bugname, responseHandler)) {
      destroy_tech_pvt(tech_pvt);
      return SWITCH_STATUS_FALSE;
    }

    if (mod_kroko_transcribe_globals.debug) {
      char filename[256] = "";
      sprintf(filename, "/tmp/fs_debug_r_%s_%d_%d.pcm", read_impl.iananame, read_impl.samples_per_second, (int)time(NULL));
      tech_pvt->fp = fopen( filename,"w");
      memset(filename,0,256);
      sprintf(filename, "/tmp/fs_debug_w_%s_%d_%d.pcm", read_impl.iananame, read_impl.samples_per_second, (int)time(NULL));
      tech_pvt->dp = fopen(filename,"w");
    }

    *ppUserData = tech_pvt;

    if (tech_pvt->pAudioPipeR) {
      kroko::AudioPipe *pAudioPipeR = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeR);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connecting now (R)\n");
      pAudioPipeR->connect();
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection in progress (R)\n");
    }

    if (tech_pvt->pAudioPipeW) {
      kroko::AudioPipe *pAudioPipeW = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeW);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connecting now (W)\n");
      pAudioPipeW->connect();
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "connection in progress (W)\n");
    }

    if (tech_pvt->pCallBack) {
      kroko::AudioPipe *pCallBack = static_cast<kroko::AudioPipe *>(tech_pvt->pCallBack);
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CallBack connecting now\n");
      pCallBack->connect();
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CallBack connection in progress\n");
    }

    return SWITCH_STATUS_SUCCESS;
  }

	switch_status_t kroko_transcribe_session_stop(switch_core_session_t *session,int channelIsClosing, char* bugname) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug = (switch_media_bug_t*) switch_channel_get_private(channel, MY_BUG_NAME);
    if (!bug) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "kroko_transcribe_session_stop: no bug - websocket conection already closed\n");
      return SWITCH_STATUS_FALSE;
    }

    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);
    if (!tech_pvt) {
      switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "tech_pvt is NULL!!!\n");
      return SWITCH_STATUS_FALSE;
    }

    uint32_t id = tech_pvt->id;
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) kroko_transcribe_session_stop\n", id);

    if (tech_pvt->pAudioPipeR) {
      kroko::AudioPipe *pAudioPipeR = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeR);
      if (tech_pvt->kroko_conn != NULL) {
        pAudioPipeR->finish();
        pAudioPipeR->close();
      }
    }

    if (tech_pvt->pAudioPipeW) {
      kroko::AudioPipe *pAudioPipeW = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeW);
      if (tech_pvt->kroko_conn != NULL) {
        pAudioPipeW->finish();
        pAudioPipeW->close();
      }
    }

    if (tech_pvt->pCallBack) {
      kroko::AudioPipe *pCallBack = static_cast<kroko::AudioPipe *>(tech_pvt->pCallBack);
      pCallBack->finish();
      pCallBack->close();
    }

    reaper(tech_pvt);

    if (tech_pvt->fp != NULL) fclose(tech_pvt->fp);
    if (tech_pvt->dp != NULL) fclose(tech_pvt->dp);

    // close connection and get final responses
    switch_mutex_lock(tech_pvt->mutex);
    switch_channel_set_private(channel, bugname, NULL);
    if (!channelIsClosing) switch_core_media_bug_remove(session, &bug);

    destroy_tech_pvt(tech_pvt);
    switch_mutex_unlock(tech_pvt->mutex);
    switch_mutex_destroy(tech_pvt->mutex);
    tech_pvt->mutex = nullptr;
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%u) kroko_transcribe_session_stop\n", id);
    return SWITCH_STATUS_SUCCESS;
  }

switch_bool_t kroko_transcribe_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    switch_frame_t frame = { 0 };
    uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];

    size_t available;
    size_t availableW;

    int16_t x;
    size_t bytes_written_float;
    float tmp_left[SWITCH_RECOMMENDED_BUFFER_SIZE];
    float tmp_right[SWITCH_RECOMMENDED_BUFFER_SIZE];

    frame.data = data;
    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

    private_t* tech_pvt = (private_t*) switch_core_media_bug_get_user_data(bug);

    if (!tech_pvt) return SWITCH_TRUE;

    if (switch_mutex_trylock(tech_pvt->mutex) == SWITCH_STATUS_SUCCESS) {
      kroko::AudioPipe *pAudioPipeR = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeR);
      if (pAudioPipeR) {
        if (pAudioPipeR->getLwsState() != kroko::AudioPipe::LWS_CLIENT_CONNECTED) {
          switch_mutex_unlock(tech_pvt->mutex);
          return SWITCH_TRUE;
        }

        pAudioPipeR->lockAudioBuffer();
        available = pAudioPipeR->binarySpaceAvailable();
      }

      kroko::AudioPipe *pAudioPipeW = static_cast<kroko::AudioPipe *>(tech_pvt->pAudioPipeW);
      if (pAudioPipeW) {
        if (pAudioPipeW->getLwsState() != kroko::AudioPipe::LWS_CLIENT_CONNECTED) {
          switch_mutex_unlock(tech_pvt->mutex);
          return SWITCH_TRUE;
        }

        pAudioPipeW->lockAudioBuffer();
        availableW = pAudioPipeW->binarySpaceAvailable();
      }

      if (NULL == tech_pvt->resampler) {
        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            memset(tmp_left, 0, SWITCH_RECOMMENDED_BUFFER_SIZE);
            memset(tmp_right, 0, SWITCH_RECOMMENDED_BUFFER_SIZE);

            if (tech_pvt->channels == 2) {
              int16_t *y = (int16_t *) frame.data;
              for (x=0; x < (frame.datalen/2); x++) {
                tmp_left[x] = (float) *(y) / 32768;
                y++;
                tmp_right[x] = (float) *(y) / 32768;
                y++;
              }

              bytes_written_float = (x * sizeof(float));

              if (tech_pvt->fp != NULL) fwrite(tmp_left, 1, bytes_written_float, tech_pvt->fp);
              if (tech_pvt->dp != NULL) fwrite(tmp_right, 1, bytes_written_float, tech_pvt->dp);
            }

            if (pAudioPipeR) {
              memcpy(pAudioPipeR->binaryWritePtr(), tmp_left, bytes_written_float);
              pAudioPipeR->binaryWritePtrAdd(bytes_written_float);

              if (available < pAudioPipeR->binaryMinSpace()) {
                if (!tech_pvt->buffer_overrun_notified) {
                  tech_pvt->buffer_overrun_notified = 1;
                  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets (R)!\n", tech_pvt->id);
                  tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
                }
                break;
              }
            }

            if (pAudioPipeW) {
              memcpy(pAudioPipeW->binaryWritePtr(), tmp_right, bytes_written_float);
              pAudioPipeW->binaryWritePtrAdd(bytes_written_float);

              if (availableW < pAudioPipeW->binaryMinSpace()) {
                if (!tech_pvt->buffer_overrun_notified) {
                  tech_pvt->buffer_overrun_notified = 1;
                  switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets (W)!\n", tech_pvt->id);
                  tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
                }
                break;
              }
            }
          }
        }
      } else {
        spx_int16_t spx_data[SWITCH_RECOMMENDED_BUFFER_SIZE];

        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
          if (frame.datalen) {
            spx_uint32_t out_len = SWITCH_RECOMMENDED_BUFFER_SIZE >> 1;
            spx_uint32_t in_len = frame.samples;

            speex_resampler_process_interleaved_int(tech_pvt->resampler, (const spx_int16_t *) frame.data, (spx_uint32_t *) &in_len, spx_data, &out_len);

            if (out_len > 0) {
              // bytes written = num samples * 2 * num channels
              size_t bytes_written = out_len << tech_pvt->channels;

              memset(tmp_left, 0, SWITCH_RECOMMENDED_BUFFER_SIZE);
              memset(tmp_right, 0, SWITCH_RECOMMENDED_BUFFER_SIZE);

              if (tech_pvt->channels == 2) {
                int16_t *y = (int16_t *) spx_data;
                for (x=0; x < out_len; x++) {
                  tmp_left[x] = (float) *(y) / 32768;
                  y++;
                  tmp_right[x] = (float) *(y) / 32768;
                  y++;
                }

                bytes_written_float = (x * sizeof(float));

                if (tech_pvt->fp != NULL) fwrite(tmp_left, 1, bytes_written_float, tech_pvt->fp);
                if (tech_pvt->dp != NULL) fwrite(tmp_right, 1, bytes_written_float, tech_pvt->dp);
              }

              if (pAudioPipeR) {
                memcpy(pAudioPipeR->binaryWritePtr(), tmp_left, bytes_written_float);
                pAudioPipeR->binaryWritePtrAdd(bytes_written_float);

                available = pAudioPipeR->binarySpaceAvailable();
                if (available < pAudioPipeR->binaryMinSpace()) {
                  if (!tech_pvt->buffer_overrun_notified) {
                    tech_pvt->buffer_overrun_notified = 1;
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets (R)!\n", tech_pvt->id);
                    tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
                  }
                  break;
                }
              }

              if (pAudioPipeW) {
                memcpy(pAudioPipeW->binaryWritePtr(), tmp_right, bytes_written_float);
                pAudioPipeW->binaryWritePtrAdd(bytes_written_float);

                availableW = pAudioPipeW->binarySpaceAvailable();
                if (availableW < pAudioPipeW->binaryMinSpace()) {
                  if (!tech_pvt->buffer_overrun_notified) {
                    tech_pvt->buffer_overrun_notified = 1;
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%u) dropping packets (W)!\n", tech_pvt->id);
                    tech_pvt->responseHandler(session, TRANSCRIBE_EVENT_BUFFER_OVERRUN, NULL, tech_pvt->bugname, 0);
                  }
                  break;
                }
              }
            }
          }
        }
      }

      if (pAudioPipeR) pAudioPipeR->unlockAudioBuffer();
      if (pAudioPipeW) pAudioPipeW->unlockAudioBuffer();

      switch_mutex_unlock(tech_pvt->mutex);
    }

    return SWITCH_TRUE;
  }
}
