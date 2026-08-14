package fr.celya.celyavox

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import com.google.firebase.messaging.FirebaseMessaging
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

class VoipMethodChannel(
    context: Context,
    messenger: BinaryMessenger,
    private val engine: VoipEngine = VoipEngine()
) : MethodChannel.MethodCallHandler {

    private val appContext: Context = context.applicationContext
    private val channel = MethodChannel(messenger, "voip_engine")
    private val provisioningManager: com.celya.voip.provisioning.ProvisioningManager?
    
    init {
        channel.setMethodCallHandler(this)
        provisioningManager = try {
            com.celya.voip.provisioning.ProvisioningManager(appContext)
        } catch (e: Exception) {
            android.util.Log.e("VoipMethodChannel", "Failed to initialize ProvisioningManager", e)
            null
        }
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "init" -> {
                    engine.initialize(appContext)
                    result.success(null)
                }
                "register" -> {
                    val username = requireArgument<String>(call, "username")
                    val password = requireArgument<String>(call, "password")
                    val domain = requireArgument<String>(call, "domain")
                    val proxy = call.argument<String>("proxy") ?: ""
                    engine.register(username, password, domain, proxy)
                    result.success(null)
                }
                "registerProvisioned" -> {
                    if (provisioningManager == null) {
                        result.error("PROVISIONING", "ProvisioningManager not available (KeyStore error - data cleared)", null)
                        return
                    }
                    val username = provisioningManager.getSipUsername()
                    val password = provisioningManager.getSipPassword()
                    val domain = provisioningManager.getSipDomain()
                    val proxy = provisioningManager.getSipProxy() ?: ""
                    if (username.isNullOrBlank() || password.isNullOrBlank() || domain.isNullOrBlank()) {
                        result.error("PROVISIONING", "Missing SIP provisioning data", null)
                        return
                    }
                    engine.register(username, password, domain, proxy)
                    result.success(null)
                }
                "unregister" -> {
                    engine.unregister()
                    result.success(null)
                }
                "makeCall" -> {
                    val callee = requireArgument<String>(call, "callee")
                    val ok = engine.startCall(callee)
                    if (!ok) {
                        result.error("CALL", "Native makeCall failed", null)
                        return
                    }
                    result.success(null)
                }
                "refreshAudio" -> {
                    val ok = engine.refreshAudio()
                    if (!ok) {
                        result.error("AUDIO", "Failed to refresh audio device", null)
                        return
                    }
                    result.success(null)
                }
                "acceptCall" -> {
                    val callId = requireArgument<String>(call, "callId")
                    engine.acceptCall(callId)
                    result.success(null)
                }
                "startInAppRinging" -> {
                    engine.startInAppRinging()
                    result.success(null)
                }
                "stopInAppRinging" -> {
                    engine.stopInAppRinging()
                    result.success(null)
                }
                "setSpeakerphone" -> {
                    val enabled = requireArgument<Boolean>(call, "enabled")
                    engine.setSpeakerphone(enabled)
                    result.success(null)
                }
                "setBluetooth" -> {
                    val enabled = requireArgument<Boolean>(call, "enabled")
                    engine.setBluetooth(enabled)
                    result.success(null)
                }
                "setMuted" -> {
                    val enabled = requireArgument<Boolean>(call, "enabled")
                    engine.setMuted(enabled)
                    result.success(null)
                }
                "isBluetoothAvailable" -> {
                    result.success(engine.isBluetoothAvailable())
                }
                "canDrawOverlays" -> {
                    result.success(Settings.canDrawOverlays(appContext))
                }
                "openOverlaySettings" -> {
                    val intent = Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION).apply {
                        data = Uri.fromParts("package", appContext.packageName, null)
                        addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    }
                    appContext.startActivity(intent)
                    result.success(null)
                }
                "getFcmToken" -> {
                    val ctx = appContext
                    val token = FcmTokenStore.getToken(ctx)
                    if (!token.isNullOrBlank()) {
                        result.success(token)
                        return
                    }
                    FirebaseMessaging.getInstance().token
                        .addOnSuccessListener { fresh ->
                            if (!fresh.isNullOrBlank()) {
                                FcmTokenStore.saveToken(ctx, fresh)
                            }
                            android.util.Log.i("VoipMethodChannel", "getFcmToken success: ${fresh?.take(8)}…")
                            result.success(fresh)
                        }
                        .addOnFailureListener { e ->
                            android.util.Log.w("VoipMethodChannel", "getFcmToken failed: ${e.message}", e)
                            result.success(null)
                        }
                }
                "sendDtmf" -> {
                    val callId = requireArgument<String>(call, "callId")
                    val digits = requireArgument<String>(call, "digits")
                    val ok = engine.sendDtmf(callId, digits)
                    if (!ok) {
                        result.error("DTMF", "Failed to send DTMF", null)
                        return
                    }
                    result.success(null)
                }
                "hangupCall" -> {
                    val callId = requireArgument<String>(call, "callId")
                    engine.endCall(callId)
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        } catch (e: IllegalArgumentException) {
            result.error("ARGUMENT", e.message, null)
        } catch (e: Exception) {
            result.error("ERROR", e.message ?: "Unknown error", null)
        }
    }

    fun dispose() {
        channel.setMethodCallHandler(null)
        engine.dispose()
    }
}

private fun <T> requireArgument(call: MethodCall, key: String): T {
    val value = call.argument<T>(key)
    return value ?: throw IllegalArgumentException("Missing required argument: $key")
}

// Lightweight engine extensions to keep MethodChannel handling compilable without SIP logic yet.
