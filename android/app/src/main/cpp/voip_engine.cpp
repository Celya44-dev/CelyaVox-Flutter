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
static std::string g_account_username = "";  // Username du compte SIP (pour auth Digest des SUBSCRIBE)
static std::string g_account_password = "";  // Password du compte SIP (pour auth Digest des SUBSCRIBE)
// Buffers statiques pour les credentials globaux (pour éviter que les pj_str_t pointent vers des buffers temporaires)
static char g_global_cred_realm_asterisk[32] = "asterisk";
static char g_global_cred_realm_wildcard[8] = "*";
static char g_global_cred_username[128] = "";
static char g_global_cred_password[128] = "";
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

// Custom PJSIP logger callback pour tracer TOUTES les trames SIP
static void pjsip_log_callback(int level, const char *data, int len) {
    // Log TOUTES les lignes PJSIP (level 3=INFO and above)
    if (data && len > 0 && level >= 3) {  // 3=INFO, 4=WARNING, 5=ERROR, 6=CRITICAL
        // Formater le log
        char log_buf[256];
        int copy_len = (len < 250) ? len : 250;
        strncpy(log_buf, data, copy_len);
        log_buf[copy_len] = '\0';
        
        // Retirer le newline final si présent
        if (copy_len > 0 && log_buf[copy_len-1] == '\n') {
            log_buf[copy_len-1] = '\0';
        }
        
        // Préfixer avec "SIP TRAME:" pour faciliter les grep
        // Colorer selon le contenu pour mieux identifier les trames importantes
        if (strstr(log_buf, "SUBSCRIBE")) {
            LOGI("=== SIP MSG [SUBSCRIBE] %s", log_buf);
        } else if (strstr(log_buf, "401") || strstr(log_buf, "Unauthorized")) {
            LOGW("=== SIP MSG [401 AUTH REQUIRED] %s", log_buf);
        } else if (strstr(log_buf, "200") || strstr(log_buf, "200 OK")) {
            LOGI("=== SIP MSG [200 OK] %s", log_buf);
        } else if (strstr(log_buf, "NOTIFY")) {
            LOGI("=== SIP MSG [NOTIFY] %s", log_buf);
        } else if (strstr(log_buf, "REGISTER") || strstr(log_buf, "registration")) {
            LOGI("=== SIP MSG [REGISTER] %s", log_buf);
        } else if (strstr(log_buf, "WWW-Authenticate") || strstr(log_buf, "Authorization")) {
            LOGW("=== SIP MSG [AUTH] %s", log_buf);
        } else if (strstr(log_buf, "SIP/2.0")) {
            // Toute ligne contenant SIP/2.0 (request ou response)
            LOGI("=== SIP MSG [SIP FRAME] %s", log_buf);
        } else if (strstr(log_buf, "pjsua") || strstr(log_buf, "evsub")) {
            // Messages PJSIP relatifs à la subscription
            LOGI("=== SIP LOG [PJSUA] %s", log_buf);
        } else {
            // Toutes les autres lignes aussi (ne pas filtrer)
            LOGI("=== SIP LOG [OTHER] %s", log_buf);
        }
    }
}

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

static std::map<pjsua_buddy_id, int> g_buddy_callback_counter;  // Track how many times on_buddy_state is called per buddy

static void on_buddy_state(pjsua_buddy_id buddy_id) {
    // Callback appelé quand l'état du buddy change
    // Ceci peut être appelé plusieurs fois: initial SENT, après 401 retry, après 200 OK, après NOTIFY
    pjsua_buddy_info buddy_info;
    pjsua_buddy_get_info(buddy_id, &buddy_info);
    
    // Compter les appels au callback
    g_buddy_callback_counter[buddy_id]++;
    int call_count = g_buddy_callback_counter[buddy_id];
    
    // Convertir sub_state en string lisible
    const char *sub_state_str = "UNKNOWN";
    switch (buddy_info.sub_state) {
        case PJSIP_EVSUB_STATE_NULL:      sub_state_str = "NULL"; break;
        case PJSIP_EVSUB_STATE_SENT:      sub_state_str = "SENT"; break;
        case PJSIP_EVSUB_STATE_ACCEPTED:  sub_state_str = "ACCEPTED"; break;
        case PJSIP_EVSUB_STATE_PENDING:   sub_state_str = "PENDING"; break;
        case PJSIP_EVSUB_STATE_ACTIVE:    sub_state_str = "ACTIVE"; break;
        case PJSIP_EVSUB_STATE_TERMINATED:sub_state_str = "TERMINATED"; break;
    }
    
    // SIP TRACE: Afficher le code de statut SIP (401, 200, etc.)
    LOGI("=== SIP TRACE: on_buddy_state CALL #%d: buddy_id=%d, sub_state=%d(%s), sip_status=%d", 
         call_count, buddy_id, buddy_info.sub_state, sub_state_str, buddy_info.status);
    
    LOGI("=== SIP TRACE: IMPORTANT: This is callback invocation #%d for this buddy (server must have responded)", call_count);
    LOGI("=== SIP TRACE: on_buddy_state DEBUG: uri=%s, monitor_pres=%d, status_text=%s",
         buddy_info.uri.ptr ? buddy_info.uri.ptr : "N/A",
         buddy_info.monitor_pres,
         buddy_info.status_text.ptr ? buddy_info.status_text.ptr : "N/A");
    
    // CRITICAL: Si status=0, cela signifie "pas de réponse SIP reçue du tout"
    // Si status=401, le serveur a refusé l'authentification
    // Si status=200, le SUBSCRIBE a réussi
    if (buddy_info.status == 0) {
        LOGW("=== SIP TRACE: status=0 (NO SIP RESPONSE RECEIVED!) - Check network/firewall");
        LOGW("=== SIP TRACE: Account credentials check:");
        LOGW("    - g_acc_id=%d", g_acc_id);
        LOGW("    - g_account_username=%s", g_account_username.c_str());
        LOGW("    - g_account_domain=%s", g_account_domain.c_str());
        LOGW("    - Buddy trying to use account %d for auth", g_acc_id);
    } else if (buddy_info.status == 401) {
        LOGW("=== SIP TRACE: RECEIVED 401 UNAUTHORIZED! Server rejected SUBSCRIBE without Digest auth");
        LOGW("=== SIP TRACE: PJSIP should retry with acc_id=%d credentials", g_acc_id);
        LOGW("=== SIP TRACE: CRITICAL: If this is the first 401 and we don't see another callback, subscription state machine is stuck");
    } else if (buddy_info.status == 200) {
        LOGI("=== SIP TRACE: RECEIVED 200 OK! Digest auth successful or not required");
    } else if (buddy_info.status > 0) {
        LOGW("=== SIP TRACE: RECEIVED SIP RESPONSE CODE %d", buddy_info.status);
    }
    
    // Parser le status de présence
    const char *presence_status = "offline";
    if (buddy_info.sub_state == PJSIP_EVSUB_STATE_ACTIVE) {
        presence_status = "available";
        LOGI(">>> on_buddy_state: Subscription ACTIVE ✓ → presence_status=available");
    } else if (buddy_info.sub_state == PJSIP_EVSUB_STATE_SENT) {
        // Si le buddy reste en SENT avec status=401, c'est un problème d'authentification
        // PJSIP devrait automatiquement retrier avec les credentials du compte (acc_id=g_acc_id)
        if (buddy_info.status == 401) {
            LOGW(">>> on_buddy_state: Buddy in SENT state with 401 Unauthorized.");
            LOGW(">>> on_buddy_state: Attempting manual pjsua_buddy_subscribe_pres to force Digest auth retry...");
            pj_status_t resubscribe_status = pjsua_buddy_subscribe_pres(buddy_id, PJ_TRUE);
            LOGI(">>> on_buddy_state: pjsua_buddy_subscribe_pres returned status=%d", resubscribe_status);
        } else if (buddy_info.status == 0) {
            LOGW(">>> on_buddy_state: Buddy in SENT state with status=0 (no response). Check network connection!");
            LOGW(">>> on_buddy_state: Device may not be reachable from app network, or firewall blocking port 5060");
        } else {
            LOGW(">>> on_buddy_state: Buddy in SENT state (status=%d), attempting pjsua_buddy_subscribe_pres...", buddy_info.status);
            pj_status_t resubscribe_status = pjsua_buddy_subscribe_pres(buddy_id, PJ_TRUE);
            LOGI(">>> on_buddy_state: pjsua_buddy_subscribe_pres returned status=%d", resubscribe_status);
        }
        LOGI(">>> on_buddy_state: Subscription NOT active (SENT) → waiting for response from server");
    } else {
        LOGI(">>> on_buddy_state: Subscription state=%s (not SENT, not ACTIVE) → monitoring...", sub_state_str);
    }
    
    // Lookup du contact depuis la reverse map
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string contact = "";
    auto it = g_buddy_reverse_map.find(buddy_id);
    if (it != g_buddy_reverse_map.end()) {
        contact = it->second;
    }
    
    // Émettre l'event
    char event_data[256];
    pj_ansi_snprintf(event_data, sizeof(event_data), "%s:%s", contact.c_str(), presence_status);
    LOGI(">>> Emitting presence_updated: %s (contact=%s, state=%s)", event_data, contact.c_str(), presence_status);
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
    log_cfg.console_level = 6;  // TRACE level (maximum verbosity - shows ALL SIP messages)
    log_cfg.level = 6;          // File level aussi - captures everything
    log_cfg.msg_logging = PJ_TRUE;  // Activer logging des messages SIP
    log_cfg.decor = PJ_LOG_HAS_SENDER | PJ_LOG_HAS_LEVEL_TEXT | PJ_LOG_HAS_MICRO_SEC;  // Include microseconds for timing

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

    // Enregistrer le callback personnalisé pour tracer les trames SIP
    LOGI("=== SIP TRACING ENABLED: Registering custom PJSIP logger callback");
    pj_log_set_log_func(&pjsip_log_callback);

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

    // CRITICAL: Copy credentials to static buffers BEFORE creating pj_str_t
    // This ensures they remain valid throughout the account lifetime
    // (JNI strings will be released after this function)
    memset(g_global_cred_username, 0, sizeof(g_global_cred_username));
    memset(g_global_cred_password, 0, sizeof(g_global_cred_password));
    strncpy(g_global_cred_username, user, sizeof(g_global_cred_username) - 1);
    strncpy(g_global_cred_password, pass, sizeof(g_global_cred_password) - 1);
    
    LOGI(">>> nativeRegister: Credentials copied to static buffers: username=%s", g_global_cred_username);

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
    
    // Ajouter 2 credentials: une pour realm spécifique et une wildcard
    // Ça garantit que PJSIP peut matcher le realm du serveur (ex: "asterisk")
    acc_cfg.cred_count = 2;
    
    // Credential 1: realm="asterisk" (pour FreePBX/Asterisk)
    // IMPORTANT: Use static buffers (g_global_cred_*) not JNI strings!
    acc_cfg.cred_info[0].realm = pj_str_t{g_global_cred_realm_asterisk, 8};
    acc_cfg.cred_info[0].scheme = pj_str_t{const_cast<char *>("digest"), 6};
    acc_cfg.cred_info[0].username = pj_str_t{g_global_cred_username, static_cast<pj_ssize_t>(strlen(g_global_cred_username))};
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = pj_str_t{g_global_cred_password, static_cast<pj_ssize_t>(strlen(g_global_cred_password))};
    
    // Credential 2: realm="*" (wildcard pour les autres realms)
    acc_cfg.cred_info[1].realm = pj_str_t{g_global_cred_realm_wildcard, 1};
    acc_cfg.cred_info[1].scheme = pj_str_t{const_cast<char *>("digest"), 6};
    acc_cfg.cred_info[1].username = pj_str_t{g_global_cred_username, static_cast<pj_ssize_t>(strlen(g_global_cred_username))};
    acc_cfg.cred_info[1].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[1].data = pj_str_t{g_global_cred_password, static_cast<pj_ssize_t>(strlen(g_global_cred_password))};

    if (proxy && std::string(proxy).length() > 0) {
        acc_cfg.proxy[0] = pj_str_t{const_cast<char *>(proxy), static_cast<pj_ssize_t>(strlen(proxy))};
        acc_cfg.proxy_cnt = 1;
    }

    pj_status_t status = pjsua_acc_add(&acc_cfg, PJ_TRUE, &g_acc_id);
    
    // DEBUG: Vérifier que g_acc_id est correctement set par pjsua_acc_add
    LOGI(">>> nativeRegister: pjsua_acc_add returned status=%d, g_acc_id=%d", status, g_acc_id);
    if (status != PJ_SUCCESS) {
        LOGE(">>> nativeRegister: Account add FAILED with status=%d", status);
        env->ReleaseStringUTFChars(juser, user);
        env->ReleaseStringUTFChars(jpass, pass);
        env->ReleaseStringUTFChars(jdomain, domain);
        env->ReleaseStringUTFChars(jproxy, proxy);
        return JNI_FALSE;
    }

    // Sauvegarder les credentials du compte pour les SUBSCRIBE (auth Digest)
    g_account_username = user;
    g_account_password = pass;
    g_account_domain = domain;
    
    LOGI(">>> nativeRegister: Account registered! username=%s, domain=%s, g_acc_id=%d (credentials from static buffers)", user, domain, g_acc_id);

    env->ReleaseStringUTFChars(juser, user);
    env->ReleaseStringUTFChars(jpass, pass);
    env->ReleaseStringUTFChars(jdomain, domain);
    env->ReleaseStringUTFChars(jproxy, proxy);
    
    // Mettre le compte en défaut pour que les buddies l'utilisent
    pjsua_acc_set_default(g_acc_id);
    LOGI(">>> nativeRegister: Account set as default for buddy SUBSCRIBE authentication");
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
    pj_status_t status = pjsua_call_hangup(call_id, 0, nullptr, nullptr);
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
    LOGI(">>> nativeSubscribePresence CALLED: contact=%s, prefix=%s, g_acc_id=%d (should be >= 0)", contact_str, prefix_str, g_acc_id);
    
    // DEBUG: Vérifier que g_acc_id est valide (non-INVALID et accessible)
    if (g_acc_id < 0) {
        LOGE(">>> nativeSubscribePresence: ERROR - g_acc_id=%d is negative/invalid!", g_acc_id);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    // DEBUG: Vérifier que le compte par défaut a les credentials
    pjsua_acc_id default_acc = pjsua_acc_get_default();
    LOGI(">>> nativeSubscribePresence: default account ID = %d, g_acc_id = %d", default_acc, g_acc_id);
    if (default_acc != g_acc_id) {
        LOGW(">>> nativeSubscribePresence: WARNING - default account (%d) != our account (%d)!", default_acc, g_acc_id);
    }
    
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Vérifier si déjà subscribé
    auto it = g_buddy_subscriptions.find(contact_str);
    if (it != g_buddy_subscriptions.end()) {
        LOGI(">>> nativeSubscribePresence: already subscribed to %s", contact_str);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_TRUE;
    }
    
    // Construire le contact final avec prefix si fourni
    std::string contact_with_prefix = std::string(contact_str);
    if (prefix_str && strlen(prefix_str) > 0 && contact_with_prefix.find(prefix_str) != 0) {
        contact_with_prefix = std::string(prefix_str) + contact_str;
    }
    LOGI(">>> nativeSubscribePresence: final_contact_with_prefix=%s", contact_with_prefix.c_str());
    
    // Construire un URI SIP valide: sip:contact@domain
    if (g_account_domain.empty()) {
        LOGE(">>> nativeSubscribePresence: account domain not available, cannot subscribe");
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    // Buffer pour l'URI SIP
    char buddy_uri_buf[256];
    pj_ansi_snprintf(buddy_uri_buf, sizeof(buddy_uri_buf), "sip:%s@%s", contact_with_prefix.c_str(), g_account_domain.c_str());
    LOGI(">>> nativeSubscribePresence: constructed buddy URI=%s", buddy_uri_buf);
    
    // Configuration du buddy pour SUBSCRIBE/NOTIFY de présence
    pjsua_buddy_config buddy_cfg;
    pjsua_buddy_config_default(&buddy_cfg);
    buddy_cfg.uri = pj_str(buddy_uri_buf);
    
    // IMPORTANT: Pour BLF (Busy Lamp Field), utiliser subscribe_dlg_event
    // Non pas subscribe (qui est pour la presence classique)
    buddy_cfg.subscribe = PJ_FALSE;             // Désactiver la presence classique
    buddy_cfg.subscribe_dlg_event = PJ_TRUE;    // Activer BLF (dialog event subscription)
    
    buddy_cfg.acc_id = g_acc_id;                // Lier le buddy au compte pour réutiliser ses credentials
    // Le buddy utilisera les credentials du compte g_acc_id pour authentifier le SUBSCRIBE après 401
    
    LOGI(">>> nativeSubscribePresence: buddy_cfg parameters:");
    char config_summary[512];
    pj_ansi_snprintf(config_summary, sizeof(config_summary), 
        "buddy_cfg SUMMARY: uri=%s, subscribe=%d, subscribe_dlg_event=%d, acc_id=%d, username=%s",
        buddy_uri_buf, buddy_cfg.subscribe, buddy_cfg.subscribe_dlg_event, buddy_cfg.acc_id, g_account_username.c_str());
    LOGI(">>> nativeSubscribePresence: %s", config_summary);
    
    // Ajouter le buddy (PJSIP envoie automatiquement SUBSCRIBE SIP au serveur)
    LOGI(">>> nativeSubscribePresence: Calling pjsua_buddy_add()...");
    pjsua_buddy_id buddy_id;
    pj_status_t status = pjsua_buddy_add(&buddy_cfg, &buddy_id);
    
    LOGI(">>> nativeSubscribePresence: pjsua_buddy_add() returned status=%d, buddy_id=%d", status, buddy_id);
    
    if (status != PJ_SUCCESS) {
        LOGE(">>> nativeSubscribePresence: pjsua_buddy_add FAILED! status=%d (PJ_SUCCESS=0)", status);
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE(">>> nativeSubscribePresence: Error message: %s", errbuf);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    // Vérifier que buddy_id est valide
    if (buddy_id < 0) {
        LOGE(">>> nativeSubscribePresence: buddy_id is INVALID (%d)! PJSUA returned PJ_SUCCESS but invalid buddy_id", buddy_id);
        env->ReleaseStringUTFChars(jcontact, contact_str);
        env->ReleaseStringUTFChars(jprefix, prefix_str);
        return JNI_FALSE;
    }
    
    LOGI(">>> nativeSubscribePresence: pjsua_buddy_add SUCCESS! buddy_id=%d is VALID", buddy_id);
    
    // Tracker la subscription dans les deux maps (subscription + reverse pour on_buddy_state lookup)
    g_buddy_subscriptions[contact_str] = buddy_id;
    g_buddy_reverse_map[buddy_id] = contact_str;  // Enable C++ lookup without JNI
    LOGI(">>> nativeSubscribePresence: Tracked in maps. SUBSCRIBE should now be sent to server for: %s", contact_str);
    
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
    g_buddy_reverse_map.erase(buddy_id);  // Also clean up reverse map
    LOGI(">>> nativeUnsubscribePresence: COMPLETE - unsubscribed from %s, cleaned reverse_map", contact_str);
    
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
