package fr.celya.celyavox

import android.app.ActivityManager
import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioManager
import android.media.AudioDeviceInfo
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.content.BroadcastReceiver
import android.content.Intent
import android.content.IntentFilter
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import com.google.firebase.messaging.FirebaseMessaging

/**
 * Bridges Flutter to native PJSIP and ConnectionService, relaying events via EventChannel.
 */
class VoipEngine(
    messenger: BinaryMessenger? = null
) : EventChannel.StreamHandler, PjsipEngine.Callback {

    private val mainHandler = Handler(Looper.getMainLooper())
    @Volatile
    private var eventSink: EventChannel.EventSink? = null
    private var eventChannel: EventChannel? = null
    private val sipEngine = PjsipEngine.instance
    @Volatile
    private var appContext: Context? = null
    private var audioDeviceCallback: AudioDeviceCallback? = null
    private var bluetoothAvailable: Boolean = false
    private var fcmReceiver: BroadcastReceiver? = null
    private var pendingFcmToken: String? = null
    private var pendingConnectedCallId: String? = null
    private var incomingRingtone: Ringtone? = null
    private var incomingVibrator: Vibrator? = null
    private val callerIdMap = mutableMapOf<String, String>()

    init {
        messenger?.let { bindEventChannel(it) }
        sipEngine.setCallback(this)
        setInstance(this)
    }

    fun bindEventChannel(messenger: BinaryMessenger) {
        synchronized(this) {
            if (eventChannel == null) {
                eventChannel = EventChannel(messenger, "voip_events").also { channel ->
                    channel.setStreamHandler(this)
                }
            }
        }
    }

    fun initialize(context: Context) {
        appContext = context.applicationContext
        // Registration can fail if MANAGE_OWN_CALLS role/permission is not granted; keep app alive.
        VoipConnectionService.registerSelfManaged(appContext!!)
        val ok = sipEngine.init()
        if (!ok) {
            Log.e(TAG, "PJSIP init failed (native lib missing or init error); continuing without SIP")
        }
        startAudioDeviceMonitoring()
        startFcmTokenMonitoring()
    }

    fun dispose() {
        eventChannel?.setStreamHandler(null)
        eventSink = null
        sipEngine.setCallback(null)
        stopAudioDeviceMonitoring()
        stopFcmTokenMonitoring()
    }

    fun register(username: String, password: String, domain: String, proxy: String) {
        sipEngine.register(username, password, domain, proxy)
    }

    fun unregister() {
        sipEngine.unregister()
    }

    fun startCall(callee: String): Boolean {
        Log.i(TAG, "VoipEngine.startCall callee=$callee")
        // Initialize audio for outgoing calls (similar to incoming calls in VoipConnection.startAudio())
        initCallAudio()
        // Activate real audio devices in PJSIP (was using null audio at app startup)
        refreshAudio()
        return sipEngine.makeCall(callee)
    }

    fun endCall(callId: String) {
        val ok = sipEngine.hangupCall(callId)
        Log.i(TAG, "VoipEngine.endCall callId=$callId ok=$ok")
        appContext?.let { ctx ->
            stopInAppRinging()
            VoipForegroundService.stop(ctx)
            try {
                ctx.sendBroadcast(Intent(ACTION_CALL_TERMINATE_REQUESTED).setPackage(ctx.packageName))
                Log.i(TAG, "Broadcasted ACTION_CALL_TERMINATE_REQUESTED")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast ACTION_CALL_TERMINATE_REQUESTED", e)
            }
            try {
                ctx.startActivity(
                    Intent(ctx, MainActivity::class.java).apply {
                        action = ACTION_CALL_TERMINATE_REQUESTED
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_ACTIVITY_CLEAR_TOP or
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                    }
                )
                Log.i(TAG, "Dispatched ACTION_CALL_TERMINATE_REQUESTED intent to MainActivity")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to dispatch ACTION_CALL_TERMINATE_REQUESTED intent", e)
            }
        }
    }

    fun acceptCall(callId: String) {
        Log.i(TAG, "VoipEngine.acceptCall callId=$callId")
        val storedCallerId = callerIdMap.getOrDefault(callId, "")
        Log.i(TAG, ">>> acceptCall: callerIdMap=$callerIdMap, storedCallerId=\"$storedCallerId\"")
        // Initialize audio for incoming calls (same as for outgoing calls)
        initCallAudio()
        // Activate real audio devices in PJSIP
        refreshAudio()
        val ok = sipEngine.acceptCall(callId)
        Log.i(TAG, "VoipEngine.acceptCall result callId=$callId ok=$ok")
    }

    fun refreshAudio(): Boolean {
        return sipEngine.refreshAudio()
    }

    fun subscribePresence(contact: String, prefix: String = "") {
        Log.i(TAG, ">>> VoipEngine.subscribePresence: contact=$contact, prefix=$prefix")
        sipEngine.subscribePresence(contact, prefix)
        Log.i(TAG, ">>> VoipEngine.subscribePresence DONE for $contact")
    }

    private fun initCallAudio() {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        
        Log.i(TAG, ">>> [OUTGOING] CALL: initCallAudio() BEGIN")
        logAudioState(audioManager, "BEFORE initCallAudio()")
        
        // Set audio mode to communication and unmute microphone
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        audioManager.isMicrophoneMute = false
        
        // Request audio focus for voice call
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val attrs = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(attrs)
                .setAcceptsDelayedFocusGain(false)
                .setOnAudioFocusChangeListener { }
                .build()
            audioManager.requestAudioFocus(request)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_VOICE_CALL,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            )
        }
        
        logAudioState(audioManager, "AFTER initCallAudio()")
        Log.i(TAG, ">>> [OUTGOING] CALL: initCallAudio() END\n")
    }

    fun setSpeakerphone(enabled: Boolean) {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        
        Log.i(TAG, ">>> setSpeakerphone($enabled) BEGIN")
        logAudioState(audioManager, "BEFORE setSpeakerphone($enabled)")
        
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        audioManager.isSpeakerphoneOn = enabled
        if (enabled && audioManager.isBluetoothScoOn) {
            audioManager.stopBluetoothSco()
            audioManager.isBluetoothScoOn = false
        }
        
        logAudioState(audioManager, "AFTER setSpeakerphone($enabled)")
        Log.i(TAG, ">>> setSpeakerphone($enabled) END\n")
    }

    fun setBluetooth(enabled: Boolean) {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        if (enabled) {
            audioManager.isSpeakerphoneOn = false
            audioManager.startBluetoothSco()
            audioManager.isBluetoothScoOn = true
        } else {
            audioManager.stopBluetoothSco()
            audioManager.isBluetoothScoOn = false
        }
    }

    fun setMuted(enabled: Boolean) {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        
        Log.i(TAG, ">>> setMuted($enabled) BEGIN")
        logAudioState(audioManager, "BEFORE setMuted($enabled)")
        audioManager.isMicrophoneMute = enabled
        logAudioState(audioManager, "AFTER setMuted($enabled)")
        Log.i(TAG, ">>> setMuted($enabled) END\n")
    }

    private fun logAudioState(audioManager: AudioManager, label: String) {
        Log.i(TAG, ">>> ============================================")
        Log.i(TAG, ">>> AUDIO STATE: $label")
        Log.i(TAG, ">>> ============================================")
        Log.i(TAG, ">>> AudioManager.mode: ${audioManager.mode} (MODE_NORMAL=${AudioManager.MODE_NORMAL}, MODE_IN_COMMUNICATION=${AudioManager.MODE_IN_COMMUNICATION}, MODE_IN_CALL=${AudioManager.MODE_IN_CALL})")
        Log.i(TAG, ">>> isSpeakerphoneOn: ${audioManager.isSpeakerphoneOn}")
        Log.i(TAG, ">>> isMicrophoneMute: ${audioManager.isMicrophoneMute}")
        Log.i(TAG, ">>> isBluetoothScoOn: ${audioManager.isBluetoothScoOn}")
        Log.i(TAG, ">>> Volume STREAM_VOICE_CALL: ${audioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL)} / Max: ${audioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL)}")
        Log.i(TAG, ">>> Volume STREAM_MUSIC: ${audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)} / Max: ${audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)}")
        Log.i(TAG, ">>> RingerMode: ${audioManager.ringerMode} (SILENT=${AudioManager.RINGER_MODE_SILENT}, VIBRATE=${AudioManager.RINGER_MODE_VIBRATE}, NORMAL=${AudioManager.RINGER_MODE_NORMAL})")
        Log.i(TAG, ">>> ============================================")
    }

    fun isBluetoothAvailable(): Boolean {
        val ctx = appContext ?: return false
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            val devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
            return devices.any { device ->
                device.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                    device.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
            }
        }
        return audioManager.isBluetoothScoAvailableOffCall
    }

    private fun startAudioDeviceMonitoring() {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            publishBluetoothAvailability(isBluetoothAvailable(), null)
            return
        }
        if (audioDeviceCallback != null) return
        audioDeviceCallback = object : AudioDeviceCallback() {
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>) {
                publishBluetoothAvailability(isBluetoothAvailable(), getBluetoothDeviceName())
            }

            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>) {
                publishBluetoothAvailability(isBluetoothAvailable(), getBluetoothDeviceName())
            }
        }
        audioManager.registerAudioDeviceCallback(audioDeviceCallback, null)
        publishBluetoothAvailability(isBluetoothAvailable(), getBluetoothDeviceName())
    }

    private fun stopAudioDeviceMonitoring() {
        val ctx = appContext ?: return
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        audioDeviceCallback?.let { audioManager.unregisterAudioDeviceCallback(it) }
        audioDeviceCallback = null
    }

    private fun publishBluetoothAvailability(available: Boolean, name: String?) {
        if (available == bluetoothAvailable && (name == null || name.isBlank())) return
        bluetoothAvailable = available
        emit(
            mapOf(
                "type" to "bluetooth_available",
                "available" to available,
                "name" to (name ?: ""),
            )
        )
    }

    private fun startFcmTokenMonitoring() {
        val ctx = appContext ?: return
        if (fcmReceiver != null) return
        fcmReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent?.action != VoipFirebaseService.ACTION_FCM_TOKEN) return
                val token = intent.getStringExtra(VoipFirebaseService.EXTRA_TOKEN) ?: return
                emit(
                    mapOf(
                        "type" to "fcm_token",
                        "token" to token,
                        "updatedAt" to System.currentTimeMillis(),
                    )
                )
            }
        }
        val filter = IntentFilter(VoipFirebaseService.ACTION_FCM_TOKEN)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            ctx.registerReceiver(fcmReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            ctx.registerReceiver(fcmReceiver, filter)
        }

        // Ensure we have a token even if onNewToken hasn't fired yet.
        FirebaseMessaging.getInstance().token
            .addOnSuccessListener { token ->
                if (!token.isNullOrBlank()) {
                    FcmTokenStore.saveToken(ctx, token)
                    Log.i(TAG, "FCM token fetched: ${token.take(8)}…")
                    if (eventSink != null) {
                        emit(
                            mapOf(
                                "type" to "fcm_token",
                                "token" to token,
                                "updatedAt" to System.currentTimeMillis(),
                            )
                        )
                    } else {
                        pendingFcmToken = token
                    }
                } else {
                    Log.w(TAG, "FCM token fetch returned empty")
                }
            }
            .addOnFailureListener { e ->
                Log.w(TAG, "Failed to fetch FCM token: ${e.message}", e)
            }

        // Emit cached token if present.
        val cached = FcmTokenStore.getToken(ctx)
        if (!cached.isNullOrBlank()) {
            if (eventSink != null) {
                emit(
                    mapOf(
                        "type" to "fcm_token",
                        "token" to cached,
                        "updatedAt" to FcmTokenStore.getUpdatedAt(ctx),
                    )
                )
            } else {
                pendingFcmToken = cached
            }
        }
    }

    private fun stopFcmTokenMonitoring() {
        val ctx = appContext ?: return
        fcmReceiver?.let { ctx.unregisterReceiver(it) }
        fcmReceiver = null
    }

    private fun getBluetoothDeviceName(): String? {
        val ctx = appContext ?: return null
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return null
        val devices = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
        val device = devices.firstOrNull { d ->
            d.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                d.type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO
        }
        return device?.productName?.toString()
    }

    fun sendDtmf(callId: String, digits: String): Boolean {
        return sipEngine.sendDtmf(callId, digits)
    }

    fun startInAppRinging() {
        val ctx = appContext
        if (ctx == null) {
            return
        }
        if (incomingRingtone != null || incomingVibrator != null) {
            return
        }
        val audioManager = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val ringerMode = audioManager.ringerMode
        if (ringerMode == AudioManager.RINGER_MODE_SILENT) {
            return
        }

        // Set audio mode for incoming call (use NORMAL to allow speaker)
        audioManager.mode = AudioManager.MODE_NORMAL
        
        // Ensure STREAM_RING volume is at maximum
        val maxVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_RING)
        audioManager.setStreamVolume(AudioManager.STREAM_RING, maxVolume, 0)

        if (ringerMode == AudioManager.RINGER_MODE_NORMAL) {
            val uri = RingtoneManager.getActualDefaultRingtoneUri(
                ctx,
                RingtoneManager.TYPE_RINGTONE
            )
            val ring = RingtoneManager.getRingtone(ctx, uri)
            if (ring != null) {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                    ring.audioAttributes = AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    ring.isLooping = true
                }
                incomingRingtone = ring
                ring.play()
            }
        }

        if (ringerMode == AudioManager.RINGER_MODE_VIBRATE) {
            val vib = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val manager = ctx.getSystemService(VibratorManager::class.java)
                manager?.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                ctx.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
            }
            incomingVibrator = vib
            if (vib != null && vib.hasVibrator()) {
                val pattern = longArrayOf(0, 500, 500)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    vib.vibrate(VibrationEffect.createWaveform(pattern, 0))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(pattern, 0)
                }
            }
        }
    }

    fun stopInAppRinging() {
        incomingRingtone?.stop()
        incomingRingtone = null
        incomingVibrator?.cancel()
        incomingVibrator = null
    }

    private fun emit(event: Map<String, Any?>) {
        val sink = eventSink ?: return
        mainHandler.post { sink.success(event) }
    }

    override fun onEvent(type: String, message: String) {
        Log.d(TAG, "Native event $type | $message")
        when (type) {
            "registration" -> {
                // Update SIP registration status based on status code
                val isRegistered = message.startsWith("200")
                sipEngine.setRegistered(isRegistered)
                Log.i(TAG, "SIP registration status updated: registered=$isRegistered ($message)")
                emit(mapOf("type" to "registration", "message" to message))
            }
            "incoming_call" -> {
                VoipFirebaseService.cancelInviteWaitFallback()
                val ctx = appContext
                if (ctx != null) {
                    VoipFirebaseService.cancelSimpleIncomingNotification(ctx)
                }
                
                // Check if SIP account is registered before processing incoming call
                val isRegistered = sipEngine.isRegistered()
                if (!isRegistered) {
                    Log.w(TAG, "SIP account not registered; ignoring incoming call")
                    return
                }
                
                VoipForegroundService.cancelNoInviteTimeout()
                if (ctx != null) {
                    // Get CallerID from SIP INVITE (message = callId)
                    val rawCallerInfo = sipEngine.getCallerInfo(message)
                    val callerId = parseCallerInfo(rawCallerInfo)
                    // Store CallerID for this call
                    callerIdMap[message] = callerId
                    Log.i(TAG, ">>> INCOMING_CALL: rawCallerInfo=\"$rawCallerInfo\", parsed callerId=\"$callerId\", callId=$message, map now: $callerIdMap")
                    
                    // Always notify Flutter about the incoming call
                    incomingCall(message, callerId)
                    
                    val ok = VoipConnectionService.startIncomingCall(ctx, message, callerId)
                    if (!ok) {
                        val now = System.currentTimeMillis()
                        val recentLaunch = now - CallActivity.lastLaunchAtMs < 10000L
                        if (CallActivity.isVisible) {
                            if (message.isNotBlank() && CallActivity.visibleCallId != message) {
                                startIncomingCallActivity(ctx, message, callerId)
                            }
                            return
                        }
                        if (recentLaunch) {
                            startIncomingCallActivity(ctx, message, callerId)
                            return
                        }
                        if (isAppInForeground(ctx) && !CallActivity.isVisible) {
                            incomingCall(message, callerId)
                            return
                        }
                        if (CallActivity.isVisible) {
                            return
                        }
                        val launched = startIncomingCallActivity(ctx, message, callerId)
                        if (!launched) {
                            incomingCall(message, callerId)
                        }
                    } else {
                    }
                } else {
                    Log.w(TAG, "appContext is null, cannot process incoming call")
                }
            }
            "outgoing_call" -> {
                emit(
                    mapOf(
                        "type" to "outgoing_call",
                        "callId" to message,
                    )
                )
            }
            "call_connected" -> {
                VoipConnectionService.markCallActive(message)
                // Reset FCM wakeup flag since call is now accepted and active
                VoipFirebaseService.setFcmWakeup(false)
                callConnected(message)
            }
            "call_ended" -> {
                val parts = message.split("|", limit = 2)
                val callId = parts.firstOrNull().orEmpty()
                val reason = parts.getOrNull(1)
                VoipConnectionService.markCallEnded(callId)
                callEnded(callId, reason)
            }
            "call_cancelled" -> {
                val ctx = appContext
                if (ctx != null) {
                    handleCallCancelled(ctx, message)
                } else {
                    Log.w(TAG, "appContext is null, cannot handle call cancellation")
                }
            }
            "presence_updated" -> {
                // Message format: "contact:status" (C++ envoie déjà le contact + status)
                Log.i(TAG, ">>> presence_updated event: message=$message")
                val parts = message.split(":", limit = 2)
                if (parts.size == 2) {
                    val contact = parts[0]
                    val status = parts[1]
                    if (contact.isNotEmpty()) {
                        Log.i(TAG, ">>> presence_updated: contact=$contact, status=$status")
                        emit(
                            mapOf(
                                "type" to "presence_state",
                                "number" to contact,
                                "state" to status,
                            )
                        )
                    } else {
                        Log.w(TAG, ">>> presence_updated: contact empty in message '$message'")
                    }
                } else {
                    Log.w(TAG, ">>> presence_updated: invalid format, expected 'contact:status', got '$message'")
                }
            }
            else -> {
                emit(mapOf("type" to type, "message" to message))
            }
        }
    }

    private fun isAppInForeground(context: Context): Boolean {
        val activityManager = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
        val running = activityManager.runningAppProcesses ?: return false
        val current = running.firstOrNull { it.processName == context.packageName } ?: return false
        return current.importance == ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND ||
            current.importance == ActivityManager.RunningAppProcessInfo.IMPORTANCE_VISIBLE
    }

    fun incomingCall(callId: String, callerId: String?) {
        Log.i(TAG, ">>> incomingCall: sending to Flutter callId=$callId, callerId=\"$callerId\"")
        emit(
            mapOf(
                "type" to "incoming_call",
                "callId" to callId,
                "callerId" to (callerId ?: ""),
            )
        )
    }

    fun callConnected(callId: String) {
        if (eventSink == null) {
            Log.w(TAG, "call_connected queued (no Flutter listener) callId=$callId")
            pendingConnectedCallId = callId
            return
        }
        Log.i(TAG, "Emitting call_connected to Flutter callId=$callId")
        val callerId = callerIdMap.getOrDefault(callId, "")
        Log.i(TAG, ">>> callConnected: callerIdMap=$callerIdMap, callerId for $callId = \"$callerId\"")
        emit(
            mapOf(
                "type" to "call_connected",
                "callId" to callId,
                "callerId" to callerId,
            )
        )
    }

    fun callEnded(callId: String, reason: String? = null) {
        appContext?.let { ctx ->
            stopInAppRinging()
            VoipForegroundService.stop(ctx)
            VoipFirebaseService.cancelInviteWaitFallback()
            VoipFirebaseService.cancelSimpleIncomingNotification(ctx)
            
            // Check if this is a call cancellation (487 Request Terminated) and app was woken by FCM
            val isRequestTerminated = reason?.contains("487") == true
            
            if (isRequestTerminated) {
                val wasWokenByFcm = VoipFirebaseService.consumeFcmWakeup()
                if (wasWokenByFcm) {
                    // Get CallerID from SIP call info and parse it
                    val rawCallerInfo = sipEngine.getCallerInfo(callId) ?: ""
                    val callerId = parseCallerInfo(rawCallerInfo)
                    VoipFirebaseService.showCancelledCallNotification(ctx, reason ?: "Appel annulé", callerId)
                    
                    // Send minimize app broadcast after a delay to allow CallActivity to close first
                    mainHandler.postDelayed(
                        {
                            try {
                                val intent = Intent(ACTION_MINIMIZE_APP).apply {
                                    setPackage(ctx.packageName)
                                }
                                ctx.sendBroadcast(intent)
                            } catch (e: Exception) {
                                Log.w(TAG, "Failed to send minimize broadcast", e)
                            }
                        },
                        500L // Wait 500ms for CallActivity to close
                    )
                }
            } else {
                // Reset FCM wakeup flag for normal call termination
                VoipFirebaseService.setFcmWakeup(false)
            }
            
            try {
                ctx.sendBroadcast(Intent(ACTION_CALL_ENDED).setPackage(ctx.packageName))
                Log.i(TAG, "Broadcasted ACTION_CALL_ENDED")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to broadcast ACTION_CALL_ENDED", e)
            }
            try {
                ctx.startActivity(
                    Intent(ctx, MainActivity::class.java).apply {
                        action = ACTION_CALL_ENDED
                        flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_ACTIVITY_CLEAR_TOP or
                            Intent.FLAG_ACTIVITY_SINGLE_TOP
                    }
                )
                Log.i(TAG, "Dispatched ACTION_CALL_ENDED intent to MainActivity")
            } catch (e: Exception) {
                Log.w(TAG, "Failed to dispatch ACTION_CALL_ENDED intent", e)
            }
        }
        // Clean up stored CallerID
        callerIdMap.remove(callId)
        emit(
            mapOf(
                "type" to "call_ended",
                "callId" to callId,
                "reason" to reason,
            )
        )
    }

    fun navigateToCallHistory() {
        Log.i(TAG, "Emitting navigate_to_call_history event to Flutter")
        emit(
            mapOf(
                "type" to "navigate_to_call_history",
            )
        )
    }

    private fun handleCallCancelled(context: Context, message: String) {
        
        // Only handle cancellation if app was woken up by FCM push
        val wasWokenByFcm = VoipFirebaseService.consumeFcmWakeup()
        if (!wasWokenByFcm) {
            return
        }
        
        // Always show notification for call cancellation after FCM wakeup
        VoipFirebaseService.showCancelledCallNotification(context, message)
        
        // Minimize the app to background
        val intent = Intent(ACTION_MINIMIZE_APP).apply {
            setPackage(context.packageName)
        }
        context.sendBroadcast(intent)
    }

    private fun startIncomingCallActivity(context: Context, callId: String, callerId: String?): Boolean {
        val intent = Intent(context, CallActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                Intent.FLAG_ACTIVITY_CLEAR_TOP or
                Intent.FLAG_ACTIVITY_SINGLE_TOP
            putExtra(CallActivity.EXTRA_CALL_ID, callId)
            putExtra(CallActivity.EXTRA_CALLER_ID, callerId)
        }
        return try {
            context.startActivity(intent)
            true
        } catch (e: Exception) {
            Log.w(TAG, "Failed to launch CallActivity", e)
            false
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
        eventSink = events
        val ctx = appContext
        val pending = pendingFcmToken
        if (ctx != null && !pending.isNullOrBlank()) {
            emit(
                mapOf(
                    "type" to "fcm_token",
                    "token" to pending,
                    "updatedAt" to FcmTokenStore.getUpdatedAt(ctx),
                )
            )
            pendingFcmToken = null
        }
        val pendingConnected = pendingConnectedCallId
        if (!pendingConnected.isNullOrBlank()) {
            Log.i(TAG, "Replaying pending call_connected to Flutter callId=$pendingConnected")
            val callerId = callerIdMap.getOrDefault(pendingConnected, "")
            emit(
                mapOf(
                    "type" to "call_connected",
                    "callId" to pendingConnected,
                    "callerId" to callerId,
                )
            )
            pendingConnectedCallId = null
        }
    }

    override fun onCancel(arguments: Any?) {
        eventSink = null
    }

    /**
     * Extrait le nom et le numéro du CallerID SIP brut.
     * Format SIP: "Display Name" <sip:number@domain> ou sip:number@domain
     * Retourne une string formatée: "Display Name" ou "Number" ou "Display Name (Number)"
     */
    private fun parseCallerInfo(rawCallerInfo: String?): String {
        if (rawCallerInfo.isNullOrBlank()) return ""
        
        val info = rawCallerInfo.trim()
        
        // Extraire le Display Name (avant <)
        val displayName = if (info.contains("<")) {
            val before = info.substringBefore("<").trim()
            // Supprimer les guillemets
            before.replace("\"", "").replace("'", "")
        } else {
            ""
        }
        
        // Extraire le numéro (entre <...> ou après sip:/tel:)
        val number = when {
            info.contains("tel:") -> {
                // Format: tel:+33612345678 ou <tel:+33612345678>
                info.substringAfter("tel:").substringBefore(">").substringBefore(";")
            }
            info.contains("sip:") -> {
                // Format: sip:0612345678@example.com ou <sip:0612345678@example.com>
                info.substringAfter("sip:").substringBefore("@").substringBefore(";")
            }
            else -> ""
        }
        
        // Retourner le résultat formaté
        return when {
            displayName.isNotBlank() && number.isNotBlank() -> "$displayName ($number)"
            displayName.isNotBlank() -> displayName
            number.isNotBlank() -> number
            else -> info
        }
    }

    companion object {
        private const val TAG = "VoipEngine"
        const val ACTION_MINIMIZE_APP = "fr.celya.celyavox.MINIMIZE_APP"
        const val ACTION_CALL_ENDED = "fr.celya.celyavox.ACTION_CALL_ENDED"
        const val ACTION_CALL_TERMINATE_REQUESTED = "fr.celya.celyavox.ACTION_CALL_TERMINATE_REQUESTED"
        private var instance: VoipEngine? = null

        @JvmStatic
        fun setInstance(voipEngine: VoipEngine) {
            instance = voipEngine
        }

        @JvmStatic
        fun startRinging() {
            instance?.startInAppRinging()
        }

        @JvmStatic
        fun stopRinging() {
            instance?.stopInAppRinging()
        }
    }
}
