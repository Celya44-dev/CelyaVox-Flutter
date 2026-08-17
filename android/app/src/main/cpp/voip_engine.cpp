#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>
#include <map>
#include <ctype.h>

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
static char g_global_proxy_with_transport[256] = "";  // Proxy URI with ;transport=udp suffix
static char g_global_acc_id[128] = "";                // Account ID URI: sip:user@domain;transport=udp
static char g_global_acc_reg_uri[128] = "";           // Registration URI: sip:domain;transport=udp
static char g_global_call_dest_uri[256] = "";         // Current call destination URI (persists for auth retry)
static std::map<std::string, pjsua_buddy_id> g_buddy_subscriptions;  // Tracker des subscriptions de présence
static std::map<pjsua_buddy_id, std::string> g_buddy_reverse_map;  // Reverse map: buddy_id → contact (pour lookup rapide)
static std::map<pjsua_buddy_id, std::string> g_buddy_last_dialog_state;  // Track last dialog state for each buddy
static jobject g_engine_instance = nullptr;  // Global reference to the Engine instance for event emission

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
        char log_buf[512];  // Increased from 256 to capture longer messages
        int copy_len = (len < 500) ? len : 500;
        strncpy(log_buf, data, copy_len);
        log_buf[copy_len] = '\0';
        
        // Retirer le newline final si présent
        if (copy_len > 0 && log_buf[copy_len-1] == '\n') {
            log_buf[copy_len-1] = '\0';
        }
        
        // Préfixer avec "SIP TRAME:" pour faciliter les grep
        // Colorer selon le contenu pour mieux identifier les trames importantes
        if (strstr(log_buf, "INVITE")) {
            LOGI("=== SIP MSG [INVITE] %s", log_buf);
        } else if (strstr(log_buf, "SUBSCRIBE")) {
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
        } else if (strstr(log_buf, "Contact")) {
            LOGW("=== SIP MSG [CONTACT] *** %s", log_buf);
        } else if (strstr(log_buf, "Via")) {
            LOGI("=== SIP MSG [VIA] %s", log_buf);
        } else if (strstr(log_buf, "Route")) {
            LOGW("=== SIP MSG [ROUTE] *** %s", log_buf);
        } else if (strstr(log_buf, "target") || strstr(log_buf, "Target") || strstr(log_buf, "server") || strstr(log_buf, "Server")) {
            LOGW("=== SIP MSG [TARGET] *** %s", log_buf);
        } else if (strstr(log_buf, "transport=") || strstr(log_buf, "Transport:") ||
                   strstr(log_buf, ";udp") || strstr(log_buf, ";tcp") || strstr(log_buf, ";tls") ||
                   strstr(log_buf, ";sctp") || strstr(log_buf, ";ws") || strstr(log_buf, ";wss")) {
            LOGW("=== SIP MSG [TRANSPORT] *** %s", log_buf);
        } else if (strstr(log_buf, "tsx") || strstr(log_buf, "tsxacb") || strstr(log_buf, "transaction")) {
            LOGI("=== SIP MSG [TRANSACTION] %s", log_buf);
        } else if (strstr(log_buf, "Unsupported") || strstr(log_buf, "PJSIP_EUNSUPTRANSPORT") ||
                   strstr(log_buf, "FAILED") || strstr(log_buf, "Error") || strstr(log_buf, "error") || 
                   strstr(log_buf, "failure") || strstr(log_buf, "Failure") ||
                   strstr(log_buf, "Temporary failure")) {
            LOGW("=== SIP MSG [ERROR] *** %s", log_buf);
        } else if (strstr(log_buf, "next server") || strstr(log_buf, "Next server") || 
                   strstr(log_buf, "will try") || strstr(log_buf, "failover")) {
            LOGW("=== SIP MSG [FAILOVER] *** %s", log_buf);
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
    
    // Convert state to readable string
    const char *state_str = "UNKNOWN";
    if (ci.state == PJSIP_INV_STATE_NULL) state_str = "NULL";
    else if (ci.state == PJSIP_INV_STATE_CALLING) state_str = "CALLING";
    else if (ci.state == PJSIP_INV_STATE_INCOMING) state_str = "INCOMING";
    else if (ci.state == PJSIP_INV_STATE_EARLY) state_str = "EARLY";
    else if (ci.state == PJSIP_INV_STATE_CONNECTING) state_str = "CONNECTING";
    else if (ci.state == PJSIP_INV_STATE_CONFIRMED) state_str = "CONFIRMED";
    else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) state_str = "DISCONNECTED";
    
    LOGI("=== CALL STATE: call_id=%d, state=%d(%s), last_status=%d, media_cnt=%u",
         call_id, ci.state, state_str, ci.last_status, ci.media_cnt);
    
    // DEBUG: Capture ALL response codes
    if (ci.last_status > 0) {
        LOGI("=== CALL STATE: RESPONSE RECEIVED - code=%d, text=%s",
             ci.last_status, 
             ci.last_status_text.ptr ? ci.last_status_text.ptr : "N/A");
    }
    
    // DEBUG: Check for 401 and capture more details
    if (ci.last_status == 401) {
        LOGW(">>> CALL STATE: *** 401 UNAUTHORIZED RECEIVED ***");
        LOGW(">>> CALL STATE: Call ID=%d, state=%s", call_id, state_str);
        LOGW(">>> CALL STATE: PJSIP should automatically retry with Digest auth from account %d", g_acc_id);
        LOGW(">>> CALL STATE: Check logs for [TRANSPORT] and [ROUTING] to see how retry is routed");
        LOGW(">>> CALL STATE: If 'Unsupported transport' error follows, server returned Contact with incompatible transport");
    }
    
    if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        LOGI("Call CONFIRMED - call_id=%d, media_cnt=%u", call_id, ci.media_cnt);
        emit_event("call_connected", std::to_string(call_id).c_str());
    } else if (ci.state == PJSIP_INV_STATE_CALLING || ci.state == PJSIP_INV_STATE_EARLY) {
        // Outgoing call is ringing (180 Ringing or 183 Session Progress)
        LOGI("Call RINGING - call_id=%d, state=%d", call_id, ci.state);
        emit_event("call_ringing", std::to_string(call_id).c_str());
    } else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) {
        LOGI("Call DISCONNECTED - call_id=%d, status=%d, reason=%s", call_id, ci.last_status,
             ci.last_status_text.ptr ? ci.last_status_text.ptr : "");
        std::string reason;
        reason += std::to_string(ci.last_status);
        reason += " ";
        reason += std::string(ci.last_status_text.ptr ? ci.last_status_text.ptr : "");
        std::string payload = std::to_string(call_id) + "|" + reason;
        emit_event("call_ended", payload.c_str());
    } else {
        LOGI("Call state change - call_id=%d, state=%d(%s) (not CONFIRMED/EARLY/DISCONNECTED)", call_id, ci.state, state_str);
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

// Helper function to map PJSUA buddy status to presence string (includes ringing, busy, etc.)
static const char* map_buddy_status_to_presence(pjsua_buddy_status status, const pj_str_t *status_text) {
    // PJSUA_BUDDY_STATUS enum values:
    // PJSUA_BUDDY_STATUS_ONLINE    = 1
    // PJSUA_BUDDY_STATUS_OFFLINE   = 2
    
    // First, check status_text for detailed presence info
    if (status_text && status_text->slen > 0) {
        char status_text_lower[256];
        int len = (status_text->slen < 255) ? (int)status_text->slen : 255;
        strncpy(status_text_lower, status_text->ptr, len);
        status_text_lower[len] = '\0';
        
        // Convert to lowercase for comparison
        for (int i = 0; i < len; i++) {
            status_text_lower[i] = tolower((unsigned char)status_text_lower[i]);
        }
        
        // Look for keywords in status_text
        if (strstr(status_text_lower, "ringing") || strstr(status_text_lower, "alerting") || strstr(status_text_lower, "calling")) {
            return "ringing";  // Incoming ringing or outgoing alerting
        }
        if (strstr(status_text_lower, "on the phone") || strstr(status_text_lower, "on_the_phone") || strstr(status_text_lower, "on_call") || strstr(status_text_lower, "confirmed")) {
            return "busy";     // Already on a call
        }
        if (strstr(status_text_lower, "away") || strstr(status_text_lower, "idle")) {
            return "away";
        }
        if (strstr(status_text_lower, "dnd") || strstr(status_text_lower, "do not disturb") || strstr(status_text_lower, "do_not_disturb")) {
            return "dnd";
        }
    }
    
    // Fall back to status enum
    switch (status) {
        case PJSUA_BUDDY_STATUS_ONLINE:   return "available";
        case PJSUA_BUDDY_STATUS_OFFLINE:  return "offline";
        default:                           return "offline";   // Unknown = offline
    }
}

// Helper function to parse dialog-info+xml and extract state
static const char* parse_dialog_state_from_xml(const char* xml_body, int xml_len) {
    if (!xml_body || xml_len <= 0) {
        LOGW(">>> parse_dialog_state_from_xml: No XML body provided");
        return nullptr;
    }
    
    // Log the received XML for debugging
    char xml_snippet[512];
    int snippet_len = (xml_len < 500) ? xml_len : 500;
    strncpy(xml_snippet, xml_body, snippet_len);
    xml_snippet[snippet_len] = '\0';
    LOGI(">>> parse_dialog_state_from_xml: Received XML (first %d bytes): %s", xml_len, xml_snippet);
    
    // Look for <state>...</state> tag
    const char* state_start = strstr(xml_body, "<state>");
    if (!state_start) {
        LOGW(">>> parse_dialog_state_from_xml: No <state> tag found in XML");
        return nullptr;
    }
    
    state_start += 7;  // Skip "<state>"
    const char* state_end = strstr(state_start, "</state>");
    if (!state_end) {
        LOGW(">>> parse_dialog_state_from_xml: No closing </state> tag found");
        return nullptr;
    }
    
    // Extract state value
    int state_len = state_end - state_start;
    static char state_value[128];
    if (state_len >= 128) {
        LOGW(">>> parse_dialog_state_from_xml: State value too long: %d", state_len);
        return nullptr;
    }
    strncpy(state_value, state_start, state_len);
    state_value[state_len] = '\0';
    
    LOGI(">>> parse_dialog_state_from_xml: Extracted dialog state: '%s'", state_value);
    
    // Map dialog state to presence state
    if (strcmp(state_value, "terminated") == 0) {
        LOGI(">>> parse_dialog_state_from_xml: Dialog terminated → presence='available' (no call)");
        return "available";
    } else if (strcmp(state_value, "early") == 0) {
        LOGI(">>> parse_dialog_state_from_xml: Dialog early → presence='ringing' (call alerting)");
        return "ringing";
    } else if (strcmp(state_value, "confirmed") == 0) {
        LOGI(">>> parse_dialog_state_from_xml: Dialog confirmed → presence='busy' (call active)");
        return "busy";
    } else {
        LOGW(">>> parse_dialog_state_from_xml: Unknown dialog state '%s', defaulting to 'available'", state_value);
        return "available";
    }
}

// PJSIP Module to intercept NOTIFY messages for dialog-info parsing
// This callback is invoked for NOTIFY requests
static pj_bool_t notify_msg_callback(pjsip_rx_data *rdata) {
    if (!rdata || !rdata->msg_info.msg) {
        return PJ_FALSE;
    }
    
    pjsip_msg *msg = rdata->msg_info.msg;
    
    // Only process NOTIFY requests
    if (msg->type != PJSIP_REQUEST_MSG || msg->line.req.method.id != PJSIP_SUBSCRIBE && msg->line.req.method.id != PJSIP_NOTIFY) {
        // Check if it's a NOTIFY by comparing method name string
        if (msg->type != PJSIP_REQUEST_MSG) return PJ_FALSE;
        pj_str_t notify_method = {"NOTIFY", 6};
        if (pj_strcmp(&msg->line.req.method.name, &notify_method) != 0) {
            return PJ_FALSE;
        }
    }
    
    LOGI(">>> NOTIFY_HANDLER: ===== NOTIFY MESSAGE RECEIVED =====");
    
    // Get the message body
    pjsip_msg_body *body = msg->body;
    if (!body || !body->data) {
        LOGW(">>> NOTIFY_HANDLER: NOTIFY has no message body");
        return PJ_FALSE;
    }
    
    // Check if this is dialog-info+xml content type
    if (!body->content_type.type.ptr || !body->content_type.subtype.ptr) {
        LOGW(">>> NOTIFY_HANDLER: No content type specified");
        return PJ_FALSE;
    }
    
    pj_str_t type = body->content_type.type;
    pj_str_t subtype = body->content_type.subtype;
    
    // Look for "application/dialog-info+xml"
    if (!(pj_stricmp2(&type, "application") == 0 && 
          (pj_stricmp2(&subtype, "dialog-info+xml") == 0 || pj_stricmp2(&subtype, "dialog-info") == 0))) {
        LOGI(">>> NOTIFY_HANDLER: Content-Type is %.*s/%.*s (not dialog-info+xml), skipping", 
             (int)type.slen, type.ptr, (int)subtype.slen, subtype.ptr);
        return PJ_FALSE;
    }
    
    LOGI(">>> NOTIFY_HANDLER: Found dialog-info+xml body, length=%u bytes", (unsigned)body->len);
    
    // Parse the XML body
    const char *xml_body = (const char *)body->data;
    int xml_len = body->len;
    
    const char *presence_state = parse_dialog_state_from_xml(xml_body, xml_len);
    if (!presence_state) {
        LOGW(">>> NOTIFY_HANDLER: Failed to parse presence state from XML");
        return PJ_FALSE;
    }
    
    // Try to extract the "uri" attribute from the first <dialog> tag to identify the buddy
    const char *uri_start = strstr(xml_body, "uri=\"");
    if (!uri_start) {
        LOGW(">>> NOTIFY_HANDLER: No uri= attribute found in XML dialog");
        return PJ_FALSE;
    }
    
    uri_start += 5;  // Skip "uri=\""
    const char *uri_end = strchr(uri_start, '"');
    if (!uri_end) {
        LOGW(">>> NOTIFY_HANDLER: No closing quote for uri= attribute");
        return PJ_FALSE;
    }
    
    // Extract contact URI (sip:username@domain)
    int uri_len = uri_end - uri_start;
    static char contact_uri[256];
    if (uri_len >= 256) {
        LOGW(">>> NOTIFY_HANDLER: Contact URI too long: %d", uri_len);
        return PJ_FALSE;
    }
    strncpy(contact_uri, uri_start, uri_len);
    contact_uri[uri_len] = '\0';
    
    LOGI(">>> NOTIFY_HANDLER: Extracted contact from XML: %s", contact_uri);
    
    // Look up the buddy_id from our subscription map
    std::lock_guard<std::mutex> lock(g_mutex);
    auto buddy_it = g_buddy_subscriptions.find(contact_uri);
    if (buddy_it == g_buddy_subscriptions.end()) {
        // Try without the sip: prefix (some NOTIFY might strip it)
        std::string contact_without_prefix = contact_uri;
        if (contact_without_prefix.substr(0, 4) == "sip:") {
            contact_without_prefix = contact_without_prefix.substr(4);
            buddy_it = g_buddy_subscriptions.find(contact_without_prefix);
            if (buddy_it == g_buddy_subscriptions.end()) {
                // Also try with sip: prefix added back
                std::string contact_with_prefix = "sip:" + contact_without_prefix;
                buddy_it = g_buddy_subscriptions.find(contact_with_prefix);
            }
        } else {
            // Try adding sip: prefix
            std::string contact_with_prefix = "sip:";
            contact_with_prefix += contact_uri;
            buddy_it = g_buddy_subscriptions.find(contact_with_prefix);
        }
        
        if (buddy_it == g_buddy_subscriptions.end()) {
            LOGW(">>> NOTIFY_HANDLER: Contact %s not found in subscription map", contact_uri);
            LOGW(">>> NOTIFY_HANDLER: Active subscriptions:");
            for (const auto &sub : g_buddy_subscriptions) {
                LOGW(">>>   - %s -> buddy_id %d", sub.first.c_str(), sub.second);
            }
            return PJ_FALSE;
        }
    }
    
    pjsua_buddy_id buddy_id = buddy_it->second;
    LOGI(">>> NOTIFY_HANDLER: Found buddy_id=%d for contact=%s", buddy_id, contact_uri);
    
    // Store the parsed presence state for this buddy
    g_buddy_last_dialog_state[buddy_id] = presence_state;
    LOGI(">>> NOTIFY_HANDLER: Updated buddy %d dialog state to '%s'", buddy_id, presence_state);
    
    // Emit presence_updated event to Dart
    std::string event_data = contact_uri;
    event_data += ":";
    event_data += presence_state;
    
    LOGI(">>> NOTIFY_HANDLER: Emitting presence_updated event: %s", event_data.c_str());
    
    JNIEnv *env = nullptr;
    if (g_vm && g_vm->AttachCurrentThread(&env, nullptr) == 0 && g_engineClass && g_engine_instance) {
        jmethodID mid = env->GetMethodID(g_engineClass, "notifyPresenceUpdated", "(Ljava/lang/String;)V");
        if (mid) {
            jstring java_event = env->NewStringUTF(event_data.c_str());
            env->CallVoidMethod(g_engine_instance, mid, java_event);
            LOGI(">>> NOTIFY_HANDLER: Presence updated event sent to Java");
            env->DeleteLocalRef(java_event);
        } else {
            LOGW(">>> NOTIFY_HANDLER: notifyPresenceUpdated method not found");
        }
    } else {
        LOGW(">>> NOTIFY_HANDLER: Cannot call Java (g_vm=%p, g_engineClass=%p, g_engine_instance=%p)", 
             (void*)g_vm, (void*)g_engineClass, (void*)g_engine_instance);
    }
    
    return PJ_FALSE;  // Don't stop processing
}

// PJSIP module definition for NOTIFY interception
static pjsip_module mod_notify_handler = {
    NULL, NULL,                    // prev, next
    { "mod-notify-handler", 18 },  // name
    -1,                            // priority
    PJSIP_MOD_PRIORITY_APPLICATION, // priority
    NULL,                          // load()
    NULL,                          // start()
    NULL,                          // stop()
    NULL,                          // unload()
    &notify_msg_callback,          // on_rx_request()
    NULL,                          // on_rx_response()
    NULL,                          // on_tx_request()
    NULL,                          // on_tx_response()
    NULL,                          // on_tsx_state()
};

static void on_buddy_state(pjsua_buddy_id buddy_id) {
    // This callback is called by PJSUA when buddy state changes
    // Log IMMEDIATELY to verify callback is being invoked at all
    __android_log_write(ANDROID_LOG_INFO, "PjsipNative", ">>> on_buddy_state: ===== CALLBACK FIRED ===== (logging BEFORE any logic)");
    LOGI(">>> on_buddy_state: ===== CALLBACK FIRED for buddy_id=%d =====", buddy_id);
    
    pjsua_buddy_info buddy_info;
    pjsua_buddy_get_info(buddy_id, &buddy_info);
    
    // Compter les appels au callback
    g_buddy_callback_counter[buddy_id]++;
    int call_count = g_buddy_callback_counter[buddy_id];
    LOGI(">>> on_buddy_state: This is call #%d for buddy_id=%d", call_count, buddy_id);
    
    // Convertir sub_state en string lisible
    const char *sub_state_str = "UNKNOWN";
    switch (buddy_info.sub_state) {
        case PJSIP_EVSUB_STATE_NULL:      sub_state_str = "NULL"; break;
        case PJSIP_EVSUB_STATE_SENT:      sub_state_str = "SENT"; break;
        case PJSIP_EVSUB_STATE_ACCEPTED:  sub_state_str = "ACCEPTED"; break;
        case PJSIP_EVSUB_STATE_PENDING:   sub_state_str = "PENDING"; break;
        case PJSIP_EVSUB_STATE_ACTIVE:    sub_state_str = "ACTIVE"; break;
        case PJSIP_EVSUB_STATE_TERMINATED:sub_state_str = "TERMINATED"; break;
        default:                           sub_state_str = "UNKNOWN"; break;
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
    // buddy_info.status contient le type pjsua_buddy_status (enum) et ne peut pas être comparé directement avec des codes HTTP
    // On se fie à sub_state pour déterminer l'état réel
    if (buddy_info.sub_state == PJSIP_EVSUB_STATE_SENT) {
        LOGI("=== SIP TRACE: sub_state=SENT (waiting for server response with credentials from acc_id=%d)", g_acc_id);
    } else if (buddy_info.sub_state == PJSIP_EVSUB_STATE_ACTIVE) {
        LOGI("=== SIP TRACE: sub_state=ACTIVE - Server accepted SUBSCRIBE ✓");
    } else if (buddy_info.sub_state == PJSIP_EVSUB_STATE_TERMINATED) {
        LOGW("=== SIP TRACE: sub_state=TERMINATED - Server ended subscription");
    }
    
    // Parser le status de présence
    const char *presence_status = "offline";
    if (buddy_info.sub_state == PJSIP_EVSUB_STATE_ACTIVE) {
        // Subscription is active - use the status field and status_text to determine presence
        presence_status = map_buddy_status_to_presence(buddy_info.status, &buddy_info.status_text);
        
        // Check if we have a more accurate dialog state from a recent NOTIFY
        // Copy the dialog state string while holding the lock to avoid use-after-free
        std::string stored_dialog_state;
        {
            std::lock_guard<std::mutex> lock_dialog(g_mutex);
            auto dialog_it = g_buddy_last_dialog_state.find(buddy_id);
            if (dialog_it != g_buddy_last_dialog_state.end() && !dialog_it->second.empty()) {
                stored_dialog_state = dialog_it->second;  // Copy the string
            }
        }
        
        if (!stored_dialog_state.empty()) {
            LOGI(">>> on_buddy_state: Found stored dialog state from NOTIFY: '%s' (overriding status-based '%s')", stored_dialog_state.c_str(), presence_status);
            presence_status = stored_dialog_state.c_str();
        }
        
        LOGI(">>> on_buddy_state: Subscription ACTIVE ✓ → presence_status=%s (buddy_status=%d)", presence_status, buddy_info.status);
        
        // Log additional debug info if available (status_text may contain extra info)
        if (buddy_info.status_text.slen > 0) {
            LOGI(">>> on_buddy_state: status_text: %.*s", (int)buddy_info.status_text.slen, buddy_info.status_text.ptr);
        }
    } else if (buddy_info.sub_state == PJSIP_EVSUB_STATE_SENT) {
        LOGI(">>> on_buddy_state: Subscription SENT - PJSIP will retry with acc_id=%d credentials if needed", g_acc_id);
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

    // Register PJSIP module to intercept NOTIFY messages
    {
        pjsip_endpoint *endpt = pjsua_get_pjsip_endpt();
        LOGI(">>> MODULE_INIT: pjsua_get_pjsip_endpt() returned: %p", (void*)endpt);
        if (endpt) {
            status = pjsip_endpt_register_module(endpt, &mod_notify_handler);
            LOGI(">>> MODULE_INIT: pjsip_endpt_register_module() returned status=%d (PJ_SUCCESS=0)", status);
            if (status == PJ_SUCCESS) {
                LOGI(">>> MODULE_INIT: ✓ PJSIP module registered successfully for NOTIFY interception");
            } else {
                LOGW(">>> MODULE_INIT: ✗ Failed to register PJSIP module: %d", status);
            }
        } else {
            LOGW(">>> MODULE_INIT: ✗ Could not get PJSIP endpoint (endpt is NULL)");
        }
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
    // Store a global reference to the Engine instance for event emission
    if (!g_engine_instance) {
        g_engine_instance = env->NewGlobalRef(obj);
        LOGI(">>> Engine instance stored globally for event emission");
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
    
    LOGI(">>> nativeRegister: Called with parameters:");
    LOGI("    - user=%s", user ? user : "NULL");
    LOGI("    - domain=%s", domain ? domain : "NULL");
    LOGI("    - proxy=%s (length=%lu)", proxy ? proxy : "NULL", proxy ? strlen(proxy) : 0);
    LOGI("    - proxy is empty? %s", (proxy == nullptr || strlen(proxy) == 0) ? "YES" : "NO");

    std::lock_guard<std::mutex> lock(g_mutex);

    // CRITICAL: Copy credentials to static buffers BEFORE creating pj_str_t
    // This ensures they remain valid throughout the account lifetime
    // (JNI strings will be released after this function)
    memset(g_global_cred_username, 0, sizeof(g_global_cred_username));
    memset(g_global_cred_password, 0, sizeof(g_global_cred_password));
    strncpy(g_global_cred_username, user, sizeof(g_global_cred_username) - 1);
    strncpy(g_global_cred_password, pass, sizeof(g_global_cred_password) - 1);
    
    LOGI(">>> nativeRegister: CRITICAL FIX - Copying JNI credentials to static buffers");
    LOGI(">>> nativeRegister: JNI user=%s, pass=%s (will be released after function)", user, pass);
    LOGI(">>> nativeRegister: Static buffer username=%s, password=%s (PERSISTENT)", g_global_cred_username, g_global_cred_password);
    LOGI(">>> nativeRegister: Buffer addresses: username_buf=%p, password_buf=%p", g_global_cred_username, g_global_cred_password);

    if (g_acc_id != PJSUA_INVALID_ID) {
        pjsua_acc_del(g_acc_id);
        g_acc_id = PJSUA_INVALID_ID;
    }

    pjsua_acc_config acc_cfg;
    pjsua_acc_config_default(&acc_cfg);

    // Use static buffers for account URIs (not temporary std::string!)
    // This prevents pj_str_t from pointing to freed memory
    memset(g_global_acc_id, 0, sizeof(g_global_acc_id));
    memset(g_global_acc_reg_uri, 0, sizeof(g_global_acc_reg_uri));
    
    // Don't add ;transport=udp to account URI - let PJSIP negotiate
    snprintf(g_global_acc_id, sizeof(g_global_acc_id) - 1, 
             "sip:%s@%s", user, domain);
    snprintf(g_global_acc_reg_uri, sizeof(g_global_acc_reg_uri) - 1, 
             "sip:%s", domain);
    
    acc_cfg.id = pj_str_t{g_global_acc_id, static_cast<pj_ssize_t>(strlen(g_global_acc_id))};
    acc_cfg.reg_uri = pj_str_t{g_global_acc_reg_uri, static_cast<pj_ssize_t>(strlen(g_global_acc_reg_uri))};
    
    LOGI(">>> nativeRegister: Account URIs (in static buffers for persistence):");
    LOGI("    - id=%s", g_global_acc_id);
    LOGI("    - reg_uri=%s", g_global_acc_reg_uri);
    acc_cfg.cred_count = 2;
    
    // Credential 1: realm="asterisk" (pour FreePBX/Asterisk)
    // IMPORTANT: Use static buffers (g_global_cred_*) not JNI strings!
    acc_cfg.cred_info[0].realm = pj_str_t{g_global_cred_realm_asterisk, 8};
    acc_cfg.cred_info[0].scheme = pj_str_t{const_cast<char *>("digest"), 6};
    acc_cfg.cred_info[0].username = pj_str_t{g_global_cred_username, static_cast<pj_ssize_t>(strlen(g_global_cred_username))};
    acc_cfg.cred_info[0].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[0].data = pj_str_t{g_global_cred_password, static_cast<pj_ssize_t>(strlen(g_global_cred_password))};
    acc_cfg.cred_info[0].algorithm_type = PJSIP_AUTH_ALGORITHM_MD5;  // PJSIP 2.17: Explicit algorithm
    
    LOGI(">>> nativeRegister: Credential[0] (realm=asterisk, algo=MD5):");
    LOGI("    - username ptr=%p, value=%s", acc_cfg.cred_info[0].username.ptr, acc_cfg.cred_info[0].username.ptr);
    LOGI("    - password ptr=%p, slen=%ld", acc_cfg.cred_info[0].data.ptr, acc_cfg.cred_info[0].data.slen);
    
    // Credential 2: realm="*" (wildcard pour les autres realms)
    acc_cfg.cred_info[1].realm = pj_str_t{g_global_cred_realm_wildcard, 1};
    acc_cfg.cred_info[1].scheme = pj_str_t{const_cast<char *>("digest"), 6};
    acc_cfg.cred_info[1].username = pj_str_t{g_global_cred_username, static_cast<pj_ssize_t>(strlen(g_global_cred_username))};
    acc_cfg.cred_info[1].data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
    acc_cfg.cred_info[1].data = pj_str_t{g_global_cred_password, static_cast<pj_ssize_t>(strlen(g_global_cred_password))};
    acc_cfg.cred_info[1].algorithm_type = PJSIP_AUTH_ALGORITHM_MD5;  // PJSIP 2.17: Explicit algorithm
    
    LOGI(">>> nativeRegister: Credential[1] (realm=wildcard, algo=MD5):");
    LOGI("    - username ptr=%p, value=%s", acc_cfg.cred_info[1].username.ptr, acc_cfg.cred_info[1].username.ptr);
    LOGI("    - password ptr=%p, slen=%ld", acc_cfg.cred_info[1].data.ptr, acc_cfg.cred_info[1].data.slen);

    if (proxy && std::string(proxy).length() > 0) {
        // Use proxy as-is without forcing transport
        // Let PJSIP negotiate transport naturally
        memset(g_global_proxy_with_transport, 0, sizeof(g_global_proxy_with_transport));
        snprintf(g_global_proxy_with_transport, sizeof(g_global_proxy_with_transport) - 1, 
                 "%s", proxy);
        acc_cfg.proxy[0] = pj_str_t{g_global_proxy_with_transport, static_cast<pj_ssize_t>(strlen(g_global_proxy_with_transport))};
        acc_cfg.proxy_cnt = 1;
        LOGI(">>> nativeRegister: Proxy CONFIGURED: %s", g_global_proxy_with_transport);
    } else {
        acc_cfg.proxy_cnt = 0;
        LOGW(">>> nativeRegister: WARNING - NO PROXY CONFIGURED! (proxy=%s, will use direct routing to domain)", proxy ? proxy : "NULL");
    }

    // PJSIP 2.17: Enable shared authentication session
    // This makes credentials available for REGISTER, INVITE, SUBSCRIBE, PUBLISH, IM, etc.
    // Ensures Digest auth retry works for all modules using account credentials
    acc_cfg.use_shared_auth = PJ_TRUE;
    LOGI(">>> nativeRegister: use_shared_auth=PJ_TRUE (PJSIP 2.17: shared auth for all modules)");

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
    
    // CRITICAL VERIFICATION: Check that account credentials are using STATIC buffers, not JNI strings
    // If they point to JNI memory, 401 retries will fail when JNI strings are released
    LOGI(">>> nativeRegister: VERIFICATION - Account created successfully:");
    LOGI("    - Account ID (g_acc_id)=%d", g_acc_id);
    LOGI("    - Credential count=%d (username=%s, password set)", acc_cfg.cred_count, g_global_cred_username);
    LOGI("    - use_shared_auth=PJ_TRUE (enables credential sharing across REGISTER/INVITE/SUBSCRIBE)");

    // Sauvegarder les credentials du compte pour les SUBSCRIBE (auth Digest)
    g_account_username = user;
    g_account_password = pass;
    g_account_domain = domain;
    
    LOGI(">>> nativeRegister: Account registered! username=%s, domain=%s, g_acc_id=%d (credentials from static buffers)", user, domain, g_acc_id);

    // DEBUG: Verify transport configuration was applied correctly
    pjsua_acc_info acc_info;
    pjsua_acc_get_info(g_acc_id, &acc_info);
    LOGI(">>> nativeRegister: TRANSPORT CONFIG VERIFICATION:");
    LOGI("    - account ID: %d", g_acc_id);
    LOGI("    - status text: %s", acc_info.status_text.ptr ? acc_info.status_text.ptr : "N/A");
    LOGI("    - has credentials (cred_count from cfg): 2");
    LOGI("    - proxy[0] with transport=udp: %s", acc_cfg.proxy_cnt > 0 ? "CONFIGURED" : "NOT CONFIGURED");
    LOGI("    - allow_contact_rewrite: 0 (disabled, force UDP routing)");
    LOGI("    - allow_via_rewrite: 0 (disabled, force UDP routing)");
    LOGI("    - use_shared_auth: PJ_TRUE (enabled)");
    LOGI(">>> nativeRegister: When INVITE 401 is received, retry should use proxy UDP routing (not server Contact)");

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
    
    // Use static buffer for call destination (CRITICAL: PJSIP needs it to persist during auth retry)
    memset(g_global_call_dest_uri, 0, sizeof(g_global_call_dest_uri));
    snprintf(g_global_call_dest_uri, sizeof(g_global_call_dest_uri) - 1, "sip:%s", number);
    
    LOGI(">>> nativeMakeCall: Destination=%s", g_global_call_dest_uri);
    
    // DEBUG: Show account configuration before INVITE
    pjsua_acc_info acc_info;
    if (pjsua_acc_get_info(g_acc_id, &acc_info) == PJ_SUCCESS) {
        LOGI(">>> nativeMakeCall: Account Config:");
        LOGI("    Account ID: %d", g_acc_id);
        LOGI("    Username: %s", g_global_cred_username);
        LOGI("    Proxy (from static buffer): %s (empty=%s)", g_global_proxy_with_transport, 
             (strlen(g_global_proxy_with_transport) == 0) ? "YES" : "NO");
        LOGI("    Account URI: %s", g_global_acc_id);
        LOGI("    Reg URI: %s", g_global_acc_reg_uri);
    }
    
    LOGI(">>> nativeMakeCall: About to send INVITE via account %d to %s", g_acc_id, g_global_call_dest_uri);
    LOGI(">>> nativeMakeCall: If 401 Unauthorized received, PJSIP should auto-retry with Digest auth");
    
    // Release JNI string NOW, before taking mutex and calling native APIs
    env->ReleaseStringUTFChars(jnumber, number);
    
    std::lock_guard<std::mutex> lock(g_mutex);
    pj_str_t dst = {g_global_call_dest_uri, static_cast<pj_ssize_t>(strlen(g_global_call_dest_uri))};
    
    pjsua_call_id call_id = PJSUA_INVALID_ID;
    pj_status_t status = pjsua_call_make_call(g_acc_id, &dst, 0, nullptr, nullptr, &call_id);
    
    LOGI(">>> nativeMakeCall: pjsua_call_make_call returned status=%d, call_id=%d", status, call_id);
    if (status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE(">>> nativeMakeCall: INVITE send FAILED immediately: %s", errbuf);
    } else {
        LOGI(">>> nativeMakeCall: INVITE sent, waiting for response (401/180/183/etc)");
    }
    
    if (status == PJMEDIA_EAUD_NODEFDEV) {
        LOGE("nativeMakeCall: No audio device. Retrying with null sound device.");
        pj_status_t null_status = pjsua_set_null_snd_dev();
        if (null_status == PJ_SUCCESS) {
            call_id = PJSUA_INVALID_ID;
            status = pjsua_call_make_call(g_acc_id, &dst, 0, nullptr, nullptr, &call_id);
            LOGI("nativeMakeCall: Retry after set_null_snd_dev status=%d, call_id=%d", status, call_id);
        }
    }
    
    if (status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE("nativeMakeCall: Failed with status %d (%s)", status, errbuf);
        emit_event("call_error", errbuf);
        return JNI_FALSE;
    }
    LOGI("nativeMakeCall: Successfully initiated call %s (id=%d, URI stored for auth retry)", g_global_call_dest_uri, call_id);
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
    
    std::lock_guard<std::mutex> lock(g_mutex);
    
    pjsua_call_info ci;
    if (pjsua_call_get_info(call_id, &ci) != PJ_SUCCESS) {
        LOGE("hangup failed: unknown call_id=%d", call_id);
        return JNI_FALSE;
    }
    
    // Log call state to help debug CANCEL issues
    const char *state_str = "UNKNOWN";
    if (ci.state == PJSIP_INV_STATE_NULL) state_str = "NULL";
    else if (ci.state == PJSIP_INV_STATE_CALLING) state_str = "CALLING";
    else if (ci.state == PJSIP_INV_STATE_INCOMING) state_str = "INCOMING";
    else if (ci.state == PJSIP_INV_STATE_EARLY) state_str = "EARLY";
    else if (ci.state == PJSIP_INV_STATE_CONNECTING) state_str = "CONNECTING";
    else if (ci.state == PJSIP_INV_STATE_CONFIRMED) state_str = "CONFIRMED";
    else if (ci.state == PJSIP_INV_STATE_DISCONNECTED) state_str = "DISCONNECTED";
    
    LOGI(">>> nativeHangupCall: call_id=%d, state=%d(%s), last_status=%d", call_id, ci.state, state_str, ci.last_status);
    
    // For non-confirmed calls (CALLING/EARLY), PJSIP will automatically send CANCEL
    // For confirmed calls, PJSIP will send BYE
    // Use code=487 to force CANCEL on non-confirmed calls; for confirmed use default (0)
    int hangup_code = 0;
    if (ci.state == PJSIP_INV_STATE_CALLING || ci.state == PJSIP_INV_STATE_EARLY) {
        LOGI(">>> nativeHangupCall: Outgoing call in %s state - will send CANCEL", state_str);
        // Use 487 Request Terminated to force CANCEL for early states
        hangup_code = 487;
    } else if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        LOGI(">>> nativeHangupCall: Call confirmed - will send BYE");
        hangup_code = 0;
    } else {
        LOGI(">>> nativeHangupCall: Call in %s state - will use default hangup behavior", state_str);
        hangup_code = 0;
    }
    
    LOGI(">>> nativeHangupCall: Using hangup code=%d", hangup_code);
    pj_status_t status = pjsua_call_hangup(call_id, hangup_code, nullptr, nullptr);
    if (status != PJ_SUCCESS) {
        char errbuf[128];
        pj_strerror(status, errbuf, sizeof(errbuf));
        LOGE(">>> nativeHangupCall: hangup failed for call_id=%d: %d (%s)", call_id, status, errbuf);
        return JNI_FALSE;
    }
    LOGI(">>> nativeHangupCall: Successfully initiated hangup for call_id=%d", call_id);
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
    
    // Construire le contact final avec prefix si fourni
    std::string contact_with_prefix = std::string(contact_str);
    if (prefix_str && strlen(prefix_str) > 0 && contact_with_prefix.find(prefix_str) != 0) {
        contact_with_prefix = std::string(prefix_str) + contact_str;
    }
    LOGI(">>> nativeSubscribePresence: final_contact_with_prefix=%s", contact_with_prefix.c_str());
    
    // Vérifier si déjà subscribé (utiliser contact_with_prefix comme clé, pas juste contact_str)
    auto it = g_buddy_subscriptions.find(contact_with_prefix);
    if (it != g_buddy_subscriptions.end()) {
        LOGI(">>> nativeSubscribePresence: already subscribed to %s", contact_with_prefix.c_str());
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
    // Note: buddy_cb doesn't exist in pjsua_buddy_config - NOTIFY messages handled by notify_msg_callback module
    
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
    
    // Tracker la subscription dans les deux maps (utiliser contact_with_prefix comme clé, PAS contact_str!)
    // Cela permet de distinguer les subscriptions au même contact avec des prefixes différents
    g_buddy_subscriptions[contact_with_prefix] = buddy_id;
    g_buddy_reverse_map[buddy_id] = contact_with_prefix;  // Stocker le contact complet avec prefix
    LOGI(">>> nativeSubscribePresence: Tracked in maps. SUBSCRIBE should now be sent to server for: %s", contact_with_prefix.c_str());
    
    env->ReleaseStringUTFChars(jcontact, contact_str);
    env->ReleaseStringUTFChars(jprefix, prefix_str);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_fr_celya_celyavox_PjsipEngine_nativeUnsubscribePresence(JNIEnv *env, jobject, jstring jcontact) {
    ensure_pj_thread_registered("jni");
    if (!ensure_endpoint()) return JNI_FALSE;
    
    const char *contact_cstr = env->GetStringUTFChars(jcontact, nullptr);
    std::string contact_str(contact_cstr);  // Convert to std::string
    LOGI(">>> nativeUnsubscribePresence CALLED: contact=%s", contact_str.c_str());
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Find subscription: either exact match OR matching contact with any prefix
    // Example: looking for "100" should find "100" or "250100" (prefix="250")
    pjsua_buddy_id buddy_id_to_delete = -1;
    std::string key_to_delete = "";
    
    for (auto& pair : g_buddy_subscriptions) {
        const std::string& key = pair.first;
        // Check exact match or if key ends with contact_str
        if (key == contact_str || (key.size() > contact_str.size() && 
            key.substr(key.size() - contact_str.size()) == contact_str)) {
            buddy_id_to_delete = pair.second;
            key_to_delete = key;
            LOGI(">>> nativeUnsubscribePresence: Found subscription key=%s matching contact=%s", key.c_str(), contact_str.c_str());
            break;
        }
    }
    
    if (buddy_id_to_delete < 0) {
        LOGW(">>> nativeUnsubscribePresence: NOT subscribed to %s", contact_str.c_str());
        env->ReleaseStringUTFChars(jcontact, contact_cstr);
        return JNI_FALSE;
    }
    
    // Supprimer le buddy (PJSIP envoie automatiquement UNSUBSCRIBE SIP)
    if (buddy_id_to_delete >= 0) {
        pj_status_t status = pjsua_buddy_del(buddy_id_to_delete);
        if (status != PJ_SUCCESS) {
            LOGE(">>> nativeUnsubscribePresence: pjsua_buddy_del FAILED for %s (buddy_id=%d, status=%d)", contact_str.c_str(), buddy_id_to_delete, status);
        } else {
            LOGI(">>> nativeUnsubscribePresence: pjsua_buddy_del SUCCESS buddy_id=%d (sending UNSUBSCRIBE to server)", buddy_id_to_delete);
        }
    }
    
    // Clean up both maps
    if (!key_to_delete.empty()) {
        g_buddy_subscriptions.erase(key_to_delete);
    }
    g_buddy_reverse_map.erase(buddy_id_to_delete);
    LOGI(">>> nativeUnsubscribePresence: COMPLETE - unsubscribed from %s, cleaned maps", contact_str.c_str());
    
    env->ReleaseStringUTFChars(jcontact, contact_cstr);
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
