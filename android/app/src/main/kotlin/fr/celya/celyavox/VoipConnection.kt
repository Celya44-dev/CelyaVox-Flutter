package fr.celya.celyavox

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.telecom.Connection
import android.telecom.DisconnectCause
import android.telecom.TelecomManager
import android.util.Log

open class VoipConnection(
    private val context: Context,
    private val callId: String? = null,
    private val callerId: String? = null
) : Connection() {

    private val audioManager: AudioManager =
        context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var audioFocusRequest: AudioFocusRequest? = null
    private var previousMode: Int? = null
    private var previousSpeakerphone: Boolean? = null
    private var previousMicMute: Boolean? = null
    private var ringtone: Ringtone? = null
    private var mediaPlayer: MediaPlayer? = null
    private var ringFocusRequest: AudioFocusRequest? = null
    private var isRinging = false
    private var vibrator: Vibrator? = null

    init {
        // Self-managed connection per Telecom requirements.
        connectionProperties = connectionProperties or PROPERTY_SELF_MANAGED
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            setAudioModeIsVoip(true)
        }
        if (!callerId.isNullOrEmpty()) {
            setCallerDisplayName(callerId, TelecomManager.PRESENTATION_ALLOWED)
        }
        setInitializing()
    }

    private fun logAudioState(label: String) {
        Log.i("VoipConnection", ">>> ============================================")
        Log.i("VoipConnection", ">>> AUDIO STATE: $label")
        Log.i("VoipConnection", ">>> ============================================")
        Log.i("VoipConnection", ">>> AudioManager.mode: ${audioManager.mode} (MODE_NORMAL=${AudioManager.MODE_NORMAL}, MODE_IN_COMMUNICATION=${AudioManager.MODE_IN_COMMUNICATION}, MODE_IN_CALL=${AudioManager.MODE_IN_CALL})")
        Log.i("VoipConnection", ">>> isSpeakerphoneOn: ${audioManager.isSpeakerphoneOn}")
        Log.i("VoipConnection", ">>> isMicrophoneMute: ${audioManager.isMicrophoneMute}")
        Log.i("VoipConnection", ">>> isBluetoothScoOn: ${audioManager.isBluetoothScoOn}")
        Log.i("VoipConnection", ">>> Volume STREAM_VOICE_CALL: ${audioManager.getStreamVolume(AudioManager.STREAM_VOICE_CALL)} / Max: ${audioManager.getStreamMaxVolume(AudioManager.STREAM_VOICE_CALL)}")
        Log.i("VoipConnection", ">>> Volume STREAM_MUSIC: ${audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)} / Max: ${audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)}")
        Log.i("VoipConnection", ">>> RingerMode: ${audioManager.ringerMode} (SILENT=${AudioManager.RINGER_MODE_SILENT}, VIBRATE=${AudioManager.RINGER_MODE_VIBRATE}, NORMAL=${AudioManager.RINGER_MODE_NORMAL})")
        Log.i("VoipConnection", ">>> callId: $callId")
        Log.i("VoipConnection", ">>> ============================================")
    }

    override fun onAnswer() {
        stopRinging()
        startAudio()
        // Synchronize with outgoing calls: refresh audio immediately like in VoipEngine.startCall()
        // This ensures consistent microphone volume for incoming calls
        PjsipEngine.instance.refreshAudio()
        setActive()
    }

    override fun onReject() {
        stopRinging()
        stopAudio()
        setDisconnected(DisconnectCause(DisconnectCause.REJECTED))
        destroy()
    }

    override fun onDisconnect() {
        stopRinging()
        stopAudio()
        setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
        destroy()
    }

    override fun onAbort() {
        stopRinging()
        stopAudio()
        setDisconnected(DisconnectCause(DisconnectCause.CANCELED))
        destroy()
    }

    fun onCallConnected() {
        stopRinging()
        startAudio()
    }

    fun stopRingingNow() {
        stopRinging()
    }

    private fun startAudio() {
        if (previousMode != null) return
        previousMode = audioManager.mode
        previousSpeakerphone = audioManager.isSpeakerphoneOn
        previousMicMute = audioManager.isMicrophoneMute

        // Log audio state BEFORE changes
        Log.i("VoipConnection", ">>> [INCOMING] CALL: startAudio() BEGIN")
        logAudioState("BEFORE startAudio()")

        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        audioManager.isMicrophoneMute = false

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
            audioFocusRequest = request
            audioManager.requestAudioFocus(request)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_VOICE_CALL,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            )
        }

        // Log audio state AFTER changes
        logAudioState("AFTER startAudio()")
        Log.i("VoipConnection", ">>> [INCOMING] CALL: startAudio() END\n")
    }

    private fun stopAudio() {
        Log.i("VoipConnection", ">>> [INCOMING] CALL: stopAudio() BEGIN")
        logAudioState("BEFORE stopAudio()")

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(null)
        }
        audioFocusRequest = null

        previousMode?.let { audioManager.mode = it }
        previousSpeakerphone?.let { audioManager.isSpeakerphoneOn = it }
        previousMicMute?.let { audioManager.isMicrophoneMute = it }
        previousMode = null
        previousSpeakerphone = null
        previousMicMute = null

        logAudioState("AFTER stopAudio()")
        Log.i("VoipConnection", ">>> [INCOMING] CALL: stopAudio() END\n")
    }

    fun markRinging() {
        setRinging()
        startRinging()
    }

    fun markDialing() {
        setDialing()
    }

    private fun startRinging() {
        if (isRinging) {
            return
        }
        isRinging = true
        val ringerMode = audioManager.ringerMode
        if (ringerMode == AudioManager.RINGER_MODE_SILENT) {
            return
        }

        // Set audio mode for incoming call (use NORMAL to allow speaker)
        audioManager.mode = AudioManager.MODE_NORMAL
        
        // Ensure stream volume is at maximum
        val maxVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_RING)
        audioManager.setStreamVolume(AudioManager.STREAM_RING, maxVolume, 0)
        
        // Request audio focus before playing
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
            ringFocusRequest = request
            audioManager.requestAudioFocus(request)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_RING,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            )
        }

        // Try MediaPlayer first
        try {
            val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
            
            val player = MediaPlayer()
            player.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            player.setDataSource(context, uri)
            player.isLooping = true
            player.setOnErrorListener { mp, what, extra ->
                false
            }
            player.setOnPreparedListener {
                it.start()
            }
            player.prepareAsync()
            mediaPlayer = player
        } catch (e: Exception) {
            // Fallback to Ringtone if MediaPlayer fails
            try {
                val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
                val ring = RingtoneManager.getRingtone(context, uri)
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
                    ringtone = ring
                    ring.play()
                }
            } catch (e2: Exception) {
                Log.e("VoipConnection", "Fallback Ringtone failed: ${e2.message}", e2)
            }
        }
        
        // Always vibrate on incoming call for better UX
        val vib = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = context.getSystemService(VibratorManager::class.java)
            manager?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator
        }
        vibrator = vib
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

    private fun stopRinging() {
        if (!isRinging) {
            return
        }
        isRinging = false
        
        // Stop MediaPlayer
        if (mediaPlayer != null) {
            try {
                if (mediaPlayer!!.isPlaying) {
                    mediaPlayer!!.stop()
                }
                mediaPlayer!!.release()
                mediaPlayer = null
            } catch (e: Exception) {
                Log.w("VoipConnection", "Error stopping MediaPlayer: ${e.message}")
            }
        }
        
        // Stop Ringtone
        ringtone?.stop()
        ringtone = null
        vibrator?.cancel()
        vibrator = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            ringFocusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(null)
        }
        ringFocusRequest = null
    }
}
