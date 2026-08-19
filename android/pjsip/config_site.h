#ifndef PJ_CONFIG_SITE_H
#define PJ_CONFIG_SITE_H

/* Define endianness for Android */
#define PJ_IS_LITTLE_ENDIAN 1
#define PJ_IS_BIG_ENDIAN 0

/* Disable video stack entirely */
#define PJMEDIA_HAS_VIDEO                 0
#define PJMEDIA_VIDEO_DEV_HAS_OPENGL      0
#define PJMEDIA_VIDEO_DEV_HAS_DSHOW       0
#define PJMEDIA_VIDEO_DEV_HAS_V4L2        0

/* Enable required audio codecs */
#define PJMEDIA_HAS_G711_CODEC            1
#define PJMEDIA_HAS_GSM_CODEC             0
/* Disable Opus to avoid missing opus headers in CI; add back when libopus is provisioned. */
#define PJMEDIA_HAS_OPUS_CODEC            0

/* Disable unused codecs for a lean mobile build */
#define PJMEDIA_HAS_G722_CODEC            0
#define PJMEDIA_HAS_L16_CODEC             0
#define PJMEDIA_HAS_SPEEX_CODEC           0
#define PJMEDIA_HAS_ILBC_CODEC            0
#define PJMEDIA_HAS_SILK_CODEC            0

/* Audio backend: use Android JNI (OpenSL ES has issues in 2.17) */
#define PJMEDIA_AUDIO_DEV_HAS_OPENSL      0
#define PJMEDIA_AUDIO_DEV_HAS_ANDROID_JNI 1
#define PJMEDIA_AUDIO_DEV_HAS_PORTAUDIO   0

/* Security / SRTP left out intentionally for minimal footprint */
#define PJMEDIA_HAS_SRTP                  0

/* Optimize for mobile VoIP */
#define PJ_ENABLE_EXTRA_CHECK             0
#define PJ_LOG_MAX_LEVEL                  5
#define PJSIP_UDP_SIZE_THRESHOLD          4000
#define PJMEDIA_ECHO_SUPPRESSOR           0
#define PJMEDIA_ECHO_USE_SIMPLE_FILTER    0

/* Disable Speex AEC explicitly */
#define PJMEDIA_HAS_SPEEX_AEC             0
#define PJMEDIA_HAS_SPEEX_AEC_PREPROCESS  0
#define PJMEDIA_HAS_SPEEX_AEC3            0

/* Disable WebRTC AEC/NS and resample to avoid external deps */
#define PJMEDIA_HAS_WEBRTC_AEC            0
#define PJMEDIA_HAS_WEBRTC_NS             0
#define PJMEDIA_RESAMPLE_IMP              PJMEDIA_RESAMPLE_NONE

/* Disable Android MediaCodec audio */
#define PJMEDIA_HAS_ANDROID_MEDIACODEC    0
#define PJMEDIA_SOUND_BUFFER_COUNT        4

/* Disable AGC/NS to prevent automatic microphone gain reduction on Android OpenSL ES */
/* AGC causes the microphone gain to drop significantly after a few seconds of call */
#define PJMEDIA_HAS_ILBC_CODEC            0
#define PJMEDIA_HAS_G729_CODEC            0

/* Enable JNI for Android audio */
#define PJ_ANDROID_JNI                     1

#endif /* PJ_CONFIG_SITE_H */
