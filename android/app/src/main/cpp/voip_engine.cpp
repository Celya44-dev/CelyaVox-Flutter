#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>
#include <map>

#include <pjlib.h>
#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjsua-lib/pjsua.h>
#include <pjmedia/audiodev.h>

#define LOG_TAG "PjsipNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM *g_vm = nullptr;
static jclass g_engineClass = nullptr;
static std::mutex g_mutex;
static bool g_initialized = false;
static pjsua_acc_id g_acc_id = PJSUA_INVALID_ID;
static bool g_audio_ready = false;
static std::string g_account_domain = "";  // Domaine du compte SIP pour construire les URI de buddy
static std::map<std::string, pjsua_buddy_id> g_buddy_subscriptions;  // Tracker des subscriptions de présence
static std::map<pjsua_buddy_id, std::string> g_buddy_reverse_map;  // Reverse map: buddy_id → contact (pour lookup rapide)

static void ensure_pj_thread_registered(const char *name) {
    if (pj_thread_is_registered()) return;
    static thread_local pj_thread_desc tls_desc;
    static thread_local pj_thread_t *tls_thread = nullptr;
    pj_bzero(&tls_desc, sizeof(tls_desc));
    pj_status_t status = pj_thread_register(name, tls_desc, &tls_thread);
    if (status != PJ_SUCCESS) {
        LOGE("pj_thread_register failed: %d", status);
    }
}

// Forward declarations
static void emit_event(const char *type, const char *message);

static JNIEnv *attach_thread(bool *did_attach) {
    *did_attach = false;
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    jint res = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) == 0) {
            *did_attach = true;
        }
    }
    return env;
}

static void detach_thread(bool did_attach) {
    if (did_attach && g_vm) {
        g_vm->DetachCurrentThread();
    }
}

static void emit_event(const char *type, const char *message) {
    bool did_attach = false;
    JNIEnv *env = attach_thread(&did_attach);
    if (!env || !g_engineClass) return;

    jmethodID mid = env->GetStaticMethodID(g_engineClass, "handleNativeEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (!mid) {
        LOGE("Failed to find handleNativeEvent");
        detach_thread(did_attach);
        return;
    }
    jstring jtype = env->NewStringUTF(type ? type : "");
    jstring jmsg = env->NewStringUTF(message ? message : "");
    env->CallStaticVoidMethod(g_engineClass, mid, jtype, jmsg);
    env->DeleteLocalRef(jtype);
    env->DeleteLocalRef(jmsg);
    detach_thread(did_attach);
}

static void on_incoming_call(pjsua_acc_id acc_id, pjsua_call_id call_id, pjsip_rx_data *rdata) {
    (void)acc_id;
    (void)rdata;
    
    LOGI("on_incoming_call: call_id=%d", call_id);
    
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        LOGI("Incoming call state=%d, media_cnt=%u", ci.state, ci.media_cnt);
    }
    
    char buf[32];
    pj_ansi_snprintf(buf, sizeof(buf), "%d", call_id);
    emit_event("incoming_call", buf);
    pjsua_call_setting opt;
    pjsua_call_setting_default(&opt);
    opt.aud_cnt = 1;
    opt.vid_cnt = 0;
    
    pj_status_t status = pjsua_call_answer(call_id, 180, nullptr, nullptr);
    LOGI("Sent 180 Ringing, status=%d", status);
}

static void on_call_state(pjsua_call_id call_id, pjsip_event *e) {
    (void)e;
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) return;
    
    LOGI("=== on_call_state call_id=%d, state=%d ===", call_id, ci.state);
    
    if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        LOGI("Call CONFIRMED - call_id=%d, media_cnt=%u", call_id, ci.media_cnt);
        emit_event("call_connected", std::to_string(call_id).c_str());
    } else if (ci.state == PJSIP_INV_STATE_CALLING || ci.state == PJSIP_INV_STATE_EARLY) {
        // Outgoing call is ringing (180 Ringing or 183 Session Progress)
        LOGI("Call RINGING - call_id=%d, state=%d", call_id, ci.state);
        emit_event("call_ringing", std::to_string(call_id).c_str());
    } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        LOGI("Call DISCONNECTED - call_id=%d, status=%d", call_id, ci.last_status);
        // Disconnect audio streams when call ends
        for (unsigned i = 0; i < ci.media_cnt; ++i) {
            if (ci.media[i].type == PJMEDIA_TYPE_AUDIO && ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
                const pjsua_conf_port_id slot = ci.media[i].stream.aud.conf_slot;
                LOGI("Disconnecting audio for call %d, slot %d", call_id, slot);
                pjsua_conf_disconnect(slot, 0);
                pjsua_conf_disconnect(0, slot);
            }
        }
        std::string reason;
        reason += std::to_string(ci.last_status);
        reason += " ";
        reason += std::string(ci.last_status_text.ptr ? ci.last_status_text.ptr : "");
        std::string payload = std::to_string(call_id) + "|" + reason;
        emit_event("call_ended", payload.c_str());
    } else {
        LOGI("Call state change - call_id=%d, state=%d (not CONFIRMED/EARLY/DISCONNECTED)", call_id, ci.state);
    }
}

static void on_call_media_state(pjsua_call_id call_id) {
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) return;
    
    LOGI("on_call_media_state: call_id=%d, state=%d, media_cnt=%u", call_id, ci.state, ci.media_cnt);
    
    for (unsigned i = 0; i < ci.media_cnt; ++i) {
        if (ci.media[i].type == PJMEDIA_TYPE_AUDIO) {
            LOGI("Media %u: type=AUDIO, status=%d", i, ci.media[i].status);
            
            if (ci.media[i].status == PJSUA_CALL_MEDIA_ACTIVE) {
                // Connect call audio to sound device (playback + capture).
                const pjsua_conf_port_id slot = ci.media[i].stream.aud.conf_slot;
                
                pj_status_t conn1 = pjsua_conf_connect(slot, 0);
                pj_status_t conn2 = pjsua_conf_connect(0, slot);
                
                LOGI("Audio connected: slot=%d, results slot->device=%d, device->slot=%d", slot, conn1, conn2);
            } else if (ci.media[i].status == PJSUA_CALL_MEDIA_ERROR) {
                LOGE("Media ERROR on call %d", call_id);
            } else {
                LOGI("Media status on call %d: %d (not active yet)", call_id, ci.media[i].status);
            }
        }
    }
}

static void on_reg_state(pjsua_acc_id acc_id) {
    pjsua_acc_info info;
    if (pjsua_acc_get_info(acc_id, &info) != PJ_SUCCESS) return;
    std::string status_text;
    if (info.status_text.ptr && info.status_text.slen > 0) {
        status_text.assign(info.status_text.ptr, info.status_text.slen);
    }
    std::string message = std::to_string(info.status);
    if (!status_text.empty()) {
        message += " ";
        message += status_text;
    }
    emit_event("registration", message.c_str());
}

static void on_buddy_state(pjsua_buddy_id buddy_id) {
    // Callback appelé quand l'état de présence d'un buddy change
    // PJSIP gère automatiquement les SUBSCRIBE/NOTIFY
    LOGI(">>> on_buddy_state CALLED: buddy_id=%d (PJSIP received NOTIFY from server)", buddy_id);
    
    // Récupérer les infos du buddy pour extraire le status de présence
    pjsua_buddy_info buddy_info;
    pjsua_buddy_get_info(buddy_id, &buddy_info);
    
    // Parser le status de présence: PJSIP_EVSUB_STATE_ACTIVE = subscription active (on reçoit les NOTIFY)
    const char *presence_status = "offline";
    LOGI(">>> on_buddy_state: buddy_id=%d, sub_state=%d, status=%d", buddy_id, buddy_info.sub_state, buddy_info.status);
    
    if (buddy_info.sub_state == PJSIP_EVSUB_STATE_ACTIVE) {
        // Subscription active = on reçoit les NOTIFY du serveur = contact AVAILABLE
        presence_status = "available";
        LOGI(">>> on_buddy_state: Subscription ACTIVE → presence_status=available");
    } else {
        // Pas de subscription active = contact OFFLINE
        presence_status = "offline";
        LOGI(">>> on_buddy_state: Subscription NOT active (sub_state=%d) → presence_status=offline", buddy_info.sub_state);
    }
    
    // Format: "buddy_id:status" pour Dart parser
    char event_data[64];
    pj_ansi_snprintf(event_data, sizeof(event_data), "%d:%s", buddy_id, presence_status);
    LOGI(">>> Emitting presence_updated event: %s", event_data);
    emit_event("presence_updated", event_data);
}

static bool ensure_endpoint() {
    ensure_pj_thread_registered("jni");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) return true;

    pj_status_t status = pjsua_create();
    if (status != PJ_SUCCESS) {
        LOGE("pjsua_create failed");
        return false;
    }

    pjsua_config ua_cfg;
    pjsua_config_default(&ua_cfg);
    ua_cfg.cb.on_incoming_call = &on_incoming_call;
    ua_cfg.cb.on_call_state = &on_call_state;
    ua_cfg.cb.on_call_media_state = &on_call_media_state;
    ua_cfg.cb.on_reg_state = &on_reg_state;
    ua_cfg.cb.on_buddy_state = &on_buddy_state;  // Callback PJSIP natif pour présence
    static const pj_str_t kUserAgent = pj_str(const_cast<char *>("CelyaVox Mobile"));
    ua_cfg.user_agent = kUserAgent;

    pjsua_logging_config log_cfg;
    pjsua_logging_config_default(&log_cfg);
    log_cfg.console_level = 4;

    pjsua_media_config media_cfg;
    pjsua_media_config_default(&media_cfg);
    media_cfg.has_ioqueue = PJ_TRUE;
    // G.711 est en 8 kHz ; le resampling est désactivé dans config_site.h.
    // Garder 8 kHz pour éviter l'échec de création de media session.
    media_cfg.clock_rate = 8000;
    media_cfg.snd_clock_rate = 8000;
    media_cfg.enable_ice = PJ_FALSE;

    status = pjsua_init(&ua_cfg, &log_cfg, &media_cfg);
    if (status != PJ_SUCCESS) {
        LOGE("pjsua_init failed: %d", status);
        pjsua_destroy();
        return false;
    }

    pjsua_transport_config trans_cfg;
    pjsua_transport_config_default(&trans_cfg);
    trans_cfg.port = 5060;
    status = pjsua_transport_create(PJSIP_TRANSPORT_UDP, &trans_cfg, nullptr);
    if (status != PJ_SUCCESS) {
        LOGE("transport create failed: %d", status);
        pjsua_destroy();
        return false;
    }

    status = pjsua_start();
    if (status != PJ_SUCCESS) {
        LOGE("pjsua_start failed: %d", status);
        pjsua_destroy();
        return false;
    }

    // Initialize with null audio device to avoid showing microphone indicator at app startup
    // Real audio devices will be set later via refreshAudio() when a call is actually made
    pj_status_t null_status = pjsua_set_null_snd_dev();
    if (null_status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(null_status, errbuf, sizeof(errbuf));
        LOGW("set_null_snd_dev failed: %d (%s)", null_status, errbuf);
    }
    g_audio_ready = true;

    g_initialized = true;
    LOGI("PJSIP initialized");
    return true;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeInit(JNIEnv *env, jobject obj) {
    ensure_pj_thread_registered("jni");
    if (!g_vm) {
        env->GetJavaVM(&g_vm);
    }
    if (!g_engineClass) {
        jclass localClass = env->GetObjectClass(obj);
        g_engineClass = static_cast<jclass>(env->NewGlobalRef(localClass));
        env->DeleteLocalRef(localClass);
    }
    return ensure_endpoint() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeRefreshAudio(JNIEnv *, jobject) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    std::lock_guard<std::mutex> lock(g_mutex);
    
    LOGI("Refreshing audio devices");
    
    // Get current audio device info
    pjmedia_aud_dev_index current_cap_dev, current_play_dev;
    pjsua_snd_get_setting(PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE, &current_play_dev);
    pjsua_snd_get_setting(PJMEDIA_AUD_DEV_CAP_INPUT_ROUTE, &current_cap_dev);
    LOGI("Current audio devices: capture=%d, playback=%d", current_cap_dev, current_play_dev);
    
    pj_status_t status = pjsua_set_snd_dev(PJMEDIA_AUD_DEFAULT_CAPTURE_DEV, PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV);
    
    LOGI("pjsua_set_snd_dev result: %d", status);
    if (status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE("Failed to set audio device: %d (%s). Falling back to null sound device.", status, errbuf);
        pj_status_t null_status = pjsua_set_null_snd_dev();
        if (null_status != PJ_SUCCESS) {
            pj_strerror(null_status, errbuf, sizeof(errbuf));
            LOGE("set_null_snd_dev also failed: %d (%s)", null_status, errbuf);
            return JNI_FALSE;
        }
    }
    
    // Get audio device info after change
    pjsua_snd_get_setting(PJMEDIA_AUD_DEV_CAP_OUTPUT_ROUTE, &current_play_dev);
    pjsua_snd_get_setting(PJMEDIA_AUD_DEV_CAP_INPUT_ROUTE, &current_cap_dev);
    LOGI("Audio devices after refresh: capture=%d, playback=%d", current_cap_dev, current_play_dev);
    
    g_audio_ready = true;
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeRegister(JNIEnv *env, jobject, jstring juser, jstring jpass, jstring jdomain, jstring jproxy) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;

    const char *user = env->GetStringUTFChars(juser, nullptr);
    const char *pass = env->GetStringUTFChars(jpass, nullptr);
    const char *domain = env->GetStringUTFChars(jdomain, nullptr);
    const char *proxy = env->GetStringUTFChars(jproxy, nullptr);

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_acc_id != PJSUA_INVALID_ID) {
        pjsua_acc_del(g_acc_id);
        g_acc_id = PJSUA_INVALID_ID;
    }

    pjsua_acc_config acc_cfg;
    pjsua_acc_config_default(&acc_cfg);

    std::string id = "sip:" + std::string(user) + "@" + std::string(domain);
    std::string reg_uri = "sip:" + std::string(domain);

    acc_cfg.id = pj_str_t{const_cast<char *>(id.c_str()), static_cast<pj_ssize_t>(strlen(id.c_str()))};
    acc_cfg.reg_uri = pj_str_t{const_cast<char *>(reg_uri.c_str()), static_cast<pj_ssize_t>(strlen(reg_uri.c_str()))};
    acc_cfg.cred_count = 1;
    acc_cfg.cred_info[0].realm = pj_str_t{const_cast<char *>("*"), 1};
    acc_cfg.cred_info[0].scheme = pj_str_t{const_cast<char *>("digest"), 6};
    acc_cfg.cred_info[0].username = pj_str_t{const_cast<char *>(user), static_cast<pj_ssize_t>(strlen(user))};
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = pj_str_t{const_cast<char *>(pass), static_cast<pj_ssize_t>(strlen(pass))};

    if (proxy && std::string(proxy).length() > 0) {
        acc_cfg.proxy[0] = pj_str_t{const_cast<char *>(proxy), static_cast<pj_ssize_t>(strlen(proxy))};
        acc_cfg.proxy_cnt = 1;
    }

    pj_status_t status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc_id);

    env->ReleaseStringUTFChars(juser, user);
    env->ReleaseStringUTFChars(jpass, pass);
    env->ReleaseStringUTFChars(jdomain, domain);
    env->ReleaseStringUTFChars(jproxy, proxy);

    if (status != PJ_SUCCESS) {
        LOGE("Account add failed: %d", status);
        return JNI_FALSE;
    }
    
    // Sauvegarder le domaine pour construire les URI de buddy
    g_account_domain = domain;
    pjsua_acc_set_default(g_acc_id);
    LOGI("Registered account id=%d", g_acc_id);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeUnregister(JNIEnv *, jobject) {
    ensure_pj_thread_registered("jni");
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_acc_id != PJSUA_INVALID_ID) {
        pj_status_t st = pjsua_acc_set_registration(g_acc_id, PJ_FALSE);
        if (st == PJ_SUCCESS) {
            LOGI("Unregister requested (REGISTER expires=0) for account id=%d", g_acc_id);
        } else {
            LOGE("Unregister request failed for account id=%d status=%d", g_acc_id, st);
        }
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeMakeCall(JNIEnv *env, jobject, jstring jnumber) {
    ensure_pj_thread_registered("jni");
    
    LOGI("nativeMakeCall: Starting outgoing call");
    
    if (!ensure_endpoint() || g_acc_id == PJSUA_INVALID_ID) {
        LOGE("nativeMakeCall: Endpoint not ready or not registered");
        return JNI_FALSE;
    }
    
    const char *number = env->GetStringUTFChars(jnumber, nullptr);
    std::string dest = "sip:" + std::string(number);
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    
    LOGI("nativeMakeCall: Making call to %s", dest.c_str());
    
    std::lock_guard<std::mutex> lock(g_mutex);
    pj_str_t dst = {const_cast<char *>(dest.c_str()), static_cast<pj_ssize_t>(strlen(dest.c_str()))};
    
    pj_status_t status = pjsua_call_make_call(g_acc_id, &dst, 0, nullptr, nullptr, &call_id);
    
    LOGI("nativeMakeCall: pjsua_call_make_call returned status=%d, call_id=%d", status, call_id);
    
    if (status == PJMEDIA_EAUD_NODEFDEV) {
        LOGE("nativeMakeCall: No audio device. Retrying with null sound device.");
        pj_status_t null_status = pjsua_set_null_snd_dev();
        if (null_status == PJ_SUCCESS) {
            call_id = PJSUA_INVALID_ID;
            status = pjsua_call_make_call(g_acc_id, &dst, 0, nullptr, nullptr, &call_id);
            LOGI("nativeMakeCall: Retry after set_null_snd_dev status=%d, call_id=%d", status, call_id);
        }
    }
    env->ReleaseStringUTFChars(jnumber, number);
    
    if (status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE("nativeMakeCall: Failed with status %d (%s)", status, errbuf);
        emit_event("call_error", errbuf);
        return JNI_FALSE;
    }
    LOGI("nativeMakeCall: Successfully initiated call %s (id=%d)", dest.c_str(), call_id);
    emit_event("outgoing_call", std::to_string(call_id).c_str());
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeAcceptCall(JNIEnv *env, jobject, jstring jcallId) {
    ensure_pj_thread_registered("jni");
    const char *cid = env->GetStringUTFChars(jcallId, nullptr);
    int call_id = atoi(cid);
    env->ReleaseStringUTFChars(jcallId, cid);
    
    LOGI("nativeAcceptCall: Answering incoming call id=%d", call_id);
    
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) == PJ_SUCCESS) {
        LOGI("nativeAcceptCall: Call state=%d, media_cnt=%u before answer", ci.state, ci.media_cnt);
    }
    
    pj_status_t status = pjsua_call_answer(call_id, 200, nullptr, nullptr);
    
    LOGI("nativeAcceptCall: pjsua_call_answer returned status=%d", status);
    
    if (status != PJ_SUCCESS) {
        LOGE("nativeAcceptCall: Failed to answer call with status %d", status);
        return JNI_FALSE;
    }
    
    LOGI("nativeAcceptCall: Successfully answered call id=%d", call_id);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeHangupCall(JNIEnv *env, jobject, jstring jcallId) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    const char *cid = env->GetStringUTFChars(jcallId, nullptr);
    int call_id = atoi(cid);
    env->ReleaseStringUTFChars(jcallId, cid);
    if (call_id < 0) {
        LOGE("hangup failed: invalid call_id=%d", call_id);
        return JNI_FALSE;
    }
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        LOGE("hangup failed: unknown call_id=%d", call_id);
        return JNI_FALSE;
    }
    pj_status_t status = pjsua_call_hangup(call_id, 480, nullptr, nullptr);
    if (status != PJ_SUCCESS) {
        LOGE("hangup failed: %d", status);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeSendDtmf(JNIEnv *env, jobject, jstring jcallId, jstring jdigits) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    const char *cid = env->GetStringUTFChars(jcallId, nullptr);
    const char *digits = env->GetStringUTFChars(jdigits, nullptr);
    int call_id = atoi(cid);
    pj_str_t dtmf = pj_str(const_cast<char *>(digits));
    std::lock_guard<std::mutex> lock(g_mutex);
    pj_status_t status = pjsua_call_dial_dtmf(call_id, &dtmf);
    env->ReleaseStringUTFChars(jcallId, cid);
    env->ReleaseStringUTFChars(jdigits, digits);
    if (status != PJ_SUCCESS) {
        LOGE("send dtmf failed: %d", status);
        return JNI_FALSE;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeGetCallerInfo(JNIEnv *env, jobject, jstring jcallId) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return nullptr;
    
    const char *cid = env->GetStringUTFChars(jcallId, nullptr);
    int call_id = atoi(cid);
    env->ReleaseStringUTFChars(jcallId, cid);
    
    pjsua_call_info ci;
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        LOGE("Failed to get call info for call_id=%d", call_id);
        return nullptr;
    }
    
    // Extract From header which contains the caller info
    if (ci.remote_info.slen > 0) {
        std::string remote_info(ci.remote_info.ptr, ci.remote_info.slen);
        jstring jresult = env->NewStringUTF(remote_info.c_str());
        return jresult;
    }
    
    return nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeSubscribePresence(JNIEnv *env, jobject, jstring jcontact, jstring jprefix) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    if (g_acc_id == PJSUA_INVALID_ID) {
        LOGW(">>> nativeSubscribePresence: account not registered yet");
        return JNI_FALSE;
    }
    
    const char *contact_str = env->GetStringUTFChars(jcontact, nullptr);
    const char *prefix_str = env->GetStringUTFChars(jprefix, nullptr);
    LOGI(">>> nativeSubscribePresence CALLED: contact=%s, prefix=%s", contact_str, prefix_str);
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Vérifier si déjà subscribé
    auto it = g_buddy_subscriptions.find(contact_str);
    if (it != g_buddy_subscriptions.end()) {
        LOGI(">>> nativeSubscribePresence: already subscribed to %s", contact_str);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_TRUE;
    }
    
    // Construire un URI SIP valide: sip:contact@domain
    if (g_account_domain.empty()) {
        LOGE(">>> nativeSubscribePresence: account domain not available, cannot subscribe");
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    // Appliquer le prefix si fourni
    std::string contact_with_prefix = std::string(contact_str);
    if (prefix_str && strlen(prefix_str) > 0 && contact_with_prefix.find(prefix_str) != 0) {
        contact_with_prefix = std::string(prefix_str) + contact_str;
    }
    LOGI(">>> nativeSubscribePresence: contact with prefix=%s", contact_with_prefix.c_str());
    
    // Buffer pour l'URI SIP
    char buddy_uri_buf[256];
    pj_ansi_snprintf(buddy_uri_buf, sizeof(buddy_uri_buf), "sip:%s@%s", contact_with_prefix.c_str(), g_account_domain.c_str());
    LOGI(">>> nativeSubscribePresence: constructed buddy URI=%s", buddy_uri_buf);
    
    // Configuration du buddy pour SUBSCRIBE/NOTIFY de présence
    pjsua_buddy_config buddy_cfg;
    pjsua_buddy_config_default(&buddy_cfg);
    buddy_cfg.uri = pj_str(buddy_uri_buf);
    buddy_cfg.subscribe = PJ_TRUE;  // Activer la subscription de présence
    
    // Ajouter le buddy (PJSIP envoie automatiquement SUBSCRIBE SIP au serveur)
    pjsua_buddy_id buddy_id;
    pj_status_t status = pjsua_buddy_add(&buddy_cfg, &buddy_id);
    if (status != PJ_SUCCESS) {
        LOGE(">>> nativeSubscribePresence: pjsua_buddy_add FAILED for %s (status=%d)", contact_str, status);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    // Tracker la subscription (dans les deux sens pour lookup rapide)
    g_buddy_subscriptions[contact_str] = buddy_id;
    g_buddy_reverse_map[buddy_id] = contact_str;  // Reverse map pour lookup buddy_id → contact
    LOGI(">>> nativeSubscribePresence: SUCCESS! buddy_id=%d, sending SUBSCRIBE to server for: %s", buddy_id, contact_str);
    
    env->ReleaseStringUTFChars(jcontact, contact_str);
    env->ReleaseStringUTFChars(jprefix, prefix_str);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeUnsubscribePresence(JNIEnv *env, jobject, jstring jcontact) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    
    const char *contact_str = env->GetStringUTFChars(jcontact, nullptr);
    LOGI(">>> nativeUnsubscribePresence CALLED: contact=%s", contact_str);
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Trouver et supprimer la subscription
    auto it = g_buddy_subscriptions.find(contact_str);
    if (it == g_buddy_subscriptions.end()) {
        LOGW(">>> nativeUnsubscribePresence: NOT subscribed to %s", contact_str);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        return JNI_FALSE;
    }
    
    pjsua_buddy_id buddy_id = it->second;
    
    // Supprimer le buddy (PJSIP envoie automatiquement UNSUBSCRIBE SIP)
    if (buddy_id >= 0) {
        pj_status_t status = pjsua_buddy_del(buddy_id);
        if (status != PJ_SUCCESS) {
            LOGE(">>> nativeUnsubscribePresence: pjsua_buddy_del FAILED for %s (buddy_id=%d, status=%d)", contact_str, buddy_id, status);
        } else {
            LOGI(">>> nativeUnsubscribePresence: pjsua_buddy_del SUCCESS buddy_id=%d (sending UNSUBSCRIBE to server)", buddy_id);
        }
    }
    
    g_buddy_subscriptions.erase(it);
    g_buddy_reverse_map.erase(buddy_id);  // Supprimer aussi de la reverse map
    LOGI(">>> nativeUnsubscribePresence: COMPLETE - unsubscribed from %s", contact_str);
    
    env->ReleaseStringUTFChars(jcontact, contact_str);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeGetPresenceStatus(JNIEnv *env, jobject, jstring jcontact) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return env->NewStringUTF("offline");
    
    const char *contact_str = env->GetStringUTFChars(jcontact, nullptr);
    LOGI(">>> nativeGetPresenceStatus CALLED: contact=%s", contact_str);
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Chercher le buddy_id
    auto it = g_buddy_subscriptions.find(contact_str);
    if (it == g_buddy_subscriptions.end()) {
        LOGW(">>> nativeGetPresenceStatus: NOT subscribed to %s", contact_str);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        return env->NewStringUTF("offline");
    }
    
    pjsua_buddy_id buddy_id = it->second;
    if (buddy_id < 0) {
        LOGW(">>> nativeGetPresenceStatus: invalid buddy_id for %s", contact_str);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        return env->NewStringUTF("offline");
    }
    
    // Récupérer les infos du buddy
    pjsua_buddy_info info;
    pj_status_t status = pjsua_buddy_get_info(buddy_id, &info);
    if (status != PJ_SUCCESS) {
        LOGE(">>> nativeGetPresenceStatus: pjsua_buddy_get_info FAILED for buddy_id=%d", buddy_id);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        return env->NewStringUTF("offline");
    }
    
    // Déterminer le statut en fonction de l'état de subscription
    const char *result = "offline";
    
    // Vérifier si la subscription est active
    if (info.sub_state == PJSIP_EVSUB_STATE_ACTIVE) {
        // Subscription active - le serveur nous a accepté la subscription
        // Utiliser un état par défaut "available"
        result = "available";
    }
    
    LOGI(">>> nativeGetPresenceStatus: buddy_id=%d, contact=%s, sub_state=%d, result=%s", buddy_id, contact_str, info.sub_state, result);
    jstring jresult = env->NewStringUTF(result);
    env->ReleaseStringUTFChars(jcontact, contact_str);
    return jresult;
}

extern "C" JNIEXPORT jstring JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeGetContactForBuddy(JNIEnv *env, jobject, jint buddy_id_param) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return env->NewStringUTF("");
    
    pjsua_buddy_id buddy_id = (pjsua_buddy_id)buddy_id_param;
    LOGI(">>> nativeGetContactForBuddy CALLED: buddy_id=%d", buddy_id);
    
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Lookup dans la reverse map
    auto it = g_buddy_reverse_map.find(buddy_id);
    if (it == g_buddy_reverse_map.end()) {
        LOGW(">>> nativeGetContactForBuddy: buddy_id %d NOT found in reverse map", buddy_id);
        return env->NewStringUTF("");
    }
    
    const std::string& contact = it->second;
    LOGI(">>> nativeGetContactForBuddy: buddy_id=%d → contact=%s", buddy_id, contact.c_str());
    return env->NewStringUTF(contact.c_str());
}
