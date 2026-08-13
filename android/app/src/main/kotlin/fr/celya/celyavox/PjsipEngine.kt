package fr.celya.celyavox

import androidx.annotation.Keep
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

class PjsipEngine private constructor() {

    interface Callback {
        fun onEvent(type: String, message: String)
    }

    companion object {
        private const val TAG = "PjsipEngine"
        val instance: PjsipEngine by lazy { PjsipEngine() }

        @Volatile
        private var libraryLoaded = false

        init {
            try {
                System.loadLibrary("voip_engine")
                libraryLoaded = true
            } catch (t: Throwable) {
                // On emulators/ABIs without the native lib, avoid crashing; callers will see init=false.
                Log.e(TAG, "Failed to load native library", t)
                libraryLoaded = false
            }
        }

        @Volatile
        private var callback: Callback? = null

        @Keep
        @JvmStatic
        fun handleNativeEvent(type: String, message: String) {
            Log.d(TAG, "Native event: $type | $message")
            callback?.onEvent(type, message)
        }
    }

    private val initialized = AtomicBoolean(false)
    private val registered = AtomicBoolean(false)

    fun setCallback(cb: Callback?) {
        callback = cb
    }

    fun isRegistered(): Boolean = registered.get()

    fun setRegistered(value: Boolean) {
        registered.set(value)
    }

    @Synchronized
    fun init(): Boolean {
        if (!libraryLoaded) {
            Log.e(TAG, "Skipping init: native lib not loaded (check ABI / jniLibs)")
            return false
        }
        if (initialized.get()) return true
        val ok = try {
            nativeInit()
        } catch (t: Throwable) {
            Log.e(TAG, "nativeInit failed", t)
            false
        }
        initialized.set(ok)
        return ok
    }

    @Synchronized
    fun register(username: String, password: String, domain: String, proxy: String = ""): Boolean {
        if (!initialized.get()) init()
        return nativeRegister(username, password, domain, proxy)
    }

    @Synchronized
    fun unregister() {
        val ready = initialized.get()
        Log.i(TAG, "unregister requested initialized=$ready")
        if (!ready) {
            Log.w(TAG, "unregister ignored: engine not initialized")
            return
        }
        nativeUnregister()
        Log.i(TAG, "unregister native call dispatched")
    }

    @Synchronized
    fun makeCall(number: String): Boolean {
        if (!initialized.get()) init()
        return nativeMakeCall(number)
    }

    @Synchronized
    fun refreshAudio(): Boolean {
        if (!initialized.get()) init()
        return nativeRefreshAudio()
    }

    @Synchronized
    fun acceptCall(callId: String): Boolean {
        val ready = initialized.get()
        Log.i(TAG, "acceptCall requested callId=$callId initialized=$ready")
        if (!ready) {
            Log.w(TAG, "acceptCall ignored: engine not initialized")
            return false
        }
        val ok = try {
            nativeAcceptCall(callId)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeAcceptCall failed for callId=$callId", t)
            false
        }
        Log.i(TAG, "acceptCall result callId=$callId ok=$ok")
        return ok
    }

    @Synchronized
    fun hangupCall(callId: String): Boolean {
        val ready = initialized.get()
        if (!ready) {
            Log.w(TAG, "hangupCall ignored: engine not initialized")
            return false
        }
        val normalized = callId.trim()
        if (normalized.isEmpty() || normalized.toIntOrNull() == null) {
            Log.w(TAG, "hangupCall ignored: invalid callId=$callId")
            return false
        }
        return try {
            nativeHangupCall(normalized)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeHangupCall failed for callId=$callId", t)
            false
        }
    }

    @Synchronized
    fun sendDtmf(callId: String, digits: String): Boolean {
        if (!initialized.get()) return false
        return nativeSendDtmf(callId, digits)
    }

    @Synchronized
    fun getCallerInfo(callId: String): String? {
        if (!initialized.get()) {
            Log.w(TAG, "getCallerInfo ignored: engine not initialized")
            return null
        }
        return try {
            val info = nativeGetCallerInfo(callId)
            if (info.isNullOrBlank()) null else info
        } catch (t: Throwable) {
            Log.w(TAG, "getCallerInfo failed for callId=$callId", t)
            null
        }
    }

    @Synchronized
    fun subscribePresence(contact: String, prefix: String = ""): Boolean {
        Log.i(TAG, ">>> PjsipEngine.subscribePresence: contact=$contact, prefix=$prefix, initialized=${initialized.get()}")
        if (!initialized.get()) init()
        val result = nativeSubscribePresence(contact, prefix)
        Log.i(TAG, ">>> PjsipEngine.subscribePresence result: $result")
        return result
    }

    @Synchronized
    fun unsubscribePresence(contact: String): Boolean {
        if (!initialized.get()) {
            Log.w(TAG, ">>> PjsipEngine.unsubscribePresence: engine not initialized")
            return false
        }
        Log.i(TAG, ">>> PjsipEngine.unsubscribePresence: contact=$contact")
        val result = nativeUnsubscribePresence(contact)
        Log.i(TAG, ">>> PjsipEngine.unsubscribePresence result: $result")
        return result
    }

    @Synchronized
    fun getPresenceStatus(contact: String): String {
        Log.i(TAG, ">>> PjsipEngine.getPresenceStatus: contact=$contact, initialized=${initialized.get()}")
        if (!initialized.get()) {
            Log.w(TAG, ">>> PjsipEngine.getPresenceStatus: engine not initialized")
            return "offline"
        }
        return nativeGetPresenceStatus(contact)
    }

    private external fun nativeInit(): Boolean
    private external fun nativeRegister(username: String, password: String, domain: String, proxy: String): Boolean
    private external fun nativeUnregister()
    private external fun nativeMakeCall(number: String): Boolean
    private external fun nativeAcceptCall(callId: String): Boolean
    private external fun nativeHangupCall(callId: String): Boolean
    private external fun nativeRefreshAudio(): Boolean
    private external fun nativeSendDtmf(callId: String, digits: String): Boolean
    private external fun nativeGetCallerInfo(callId: String): String?
    private external fun nativeSubscribePresence(contact: String, prefix: String): Boolean
    private external fun nativeUnsubscribePresence(contact: String): Boolean
    private external fun nativeGetPresenceStatus(contact: String): String
}
