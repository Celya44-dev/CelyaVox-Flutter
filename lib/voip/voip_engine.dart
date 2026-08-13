import 'package:flutter/services.dart';

/// Flutter-facing VoIP bridge using a platform MethodChannel.
class VoipEngine {
  const VoipEngine();

  static const MethodChannel _channel = MethodChannel('voip_engine');

  Future<void> init() => _invoke('init');

  Future<void> register(
    String username,
    String password,
    String domain,
    String proxy,
  ) =>
      _invoke('register', <String, dynamic>{
        'username': username,
        'password': password,
        'domain': domain,
        'proxy': proxy,
      });

  Future<void> registerProvisioned() => _invoke('registerProvisioned');

  Future<void> unregister() => _invoke('unregister');

  Future<void> makeCall(String callee) =>
      _invoke('makeCall', <String, dynamic>{'callee': callee});

  Future<void> refreshAudio() => _invoke('refreshAudio');

  Future<void> acceptCall(String callId) =>
      _invoke('acceptCall', <String, dynamic>{'callId': callId});

  Future<void> startInAppRinging({bool isOutgoing = false}) => _invoke('startInAppRinging', <String, dynamic>{'isOutgoing': isOutgoing});

  Future<void> stopInAppRinging() => _invoke('stopInAppRinging');

  Future<void> setSpeakerphone(bool enabled) =>
      _invoke('setSpeakerphone', <String, dynamic>{'enabled': enabled});

  Future<void> setBluetooth(bool enabled) =>
      _invoke('setBluetooth', <String, dynamic>{'enabled': enabled});

  Future<bool> isBluetoothAvailable() async {
    final result = await _invoke('isBluetoothAvailable');
    return (result as bool?) ?? false;
  }

  Future<bool> canDrawOverlays() async {
    final result = await _invoke('canDrawOverlays');
    return (result as bool?) ?? false;
  }

  Future<void> openOverlaySettings() => _invoke('openOverlaySettings');

  Future<String?> getFcmToken() async {
    final result = await _invoke('getFcmToken');
    return result as String?;
  }

  Future<void> setMuted(bool enabled) =>
      _invoke('setMuted', <String, dynamic>{'enabled': enabled});

  Future<void> sendDtmf(String callId, String digits) =>
      _invoke('sendDtmf', <String, dynamic>{
      'callId': callId,
      'digits': digits,
      });

  Future<void> hangupCall(String callId) =>
      _invoke('hangupCall', <String, dynamic>{'callId': callId});

  Future<void> subscribePresence(String contact) =>
      _invoke('subscribePresence', <String, dynamic>{'contact': contact});

  Future<void> unsubscribePresence(String contact) =>
      _invoke('unsubscribePresence', <String, dynamic>{'contact': contact});

  Future<String> getPresenceStatus(String contact) async {
    final result = await _invoke('getPresenceStatus', <String, dynamic>{'contact': contact});
    return (result as String?) ?? 'offline';
  }

  Future<dynamic> _invoke(String method, [Map<String, dynamic>? arguments]) async {
    try {
      return await _channel.invokeMethod<dynamic>(method, arguments);
    } on PlatformException catch (e) {
      final message = e.message ?? e.code;
      throw Exception('VoIP method "$method" failed: $message');
    } catch (e) {
      throw Exception('VoIP method "$method" failed: $e');
    }
  }
}
