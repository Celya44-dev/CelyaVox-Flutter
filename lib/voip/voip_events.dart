import 'dart:async';
import 'dart:developer' as developer;

import 'package:flutter/services.dart';

const EventChannel _eventChannel = EventChannel('voip_events');

/// Base class for all VoIP platform events.
sealed class VoipEvent {
  const VoipEvent();

  static VoipEvent fromMap(Map<dynamic, dynamic> map) {
    final type = map['type'] as String?;
    switch (type) {
      case 'incoming_call':
        final callId = map['callId'] as String? ?? '';
        final callerId = map['callerId'] as String? ?? '';
        developer.log('INCOMING_CALL: callId=$callId, callerId=$callerId', name: 'VoipEvents');
        return IncomingCallEvent(
          callId: callId,
          callerId: callerId,
        );
      case 'registration':
        return RegistrationEvent(
          statusText: map['message'] as String? ?? '',
        );
      case 'call_connected':
        return CallConnectedEvent(
          callId: map['callId'] as String? ?? '',
          callerId: map['callerId'] as String? ?? '',
        );
      case 'outgoing_call':
        return OutgoingCallEvent(
          callId: map['callId'] as String? ?? '',
        );
      case 'call_ringing':
        return CallRingingEvent(
          callId: map['callId'] as String? ?? '',
        );
      case 'call_ended':
        return CallEndedEvent(
          callId: map['callId'] as String? ?? '',
          reason: map['reason'] as String?,
        );
      case 'bluetooth_available':
        return BluetoothAvailabilityEvent(
          available: map['available'] as bool? ?? false,
          name: map['name'] as String? ?? '',
        );
      case 'fcm_token':
        return FcmTokenEvent(
          token: map['token'] as String? ?? '',
          updatedAt: map['updatedAt'] as int? ?? 0,
        );
      case 'navigate_to_call_history':
        return NavigateToCallHistoryEvent();
      case 'presence_state':
        return PresenceStateEvent(
          number: map['number'] as String? ?? '',
          state: map['state'] as String? ?? 'offline',
        );
      default:
        throw PlatformException(
          code: 'UNKNOWN_EVENT',
          message: 'Unknown VoIP event type: $type',
        );
    }
  }
}

class IncomingCallEvent extends VoipEvent {
  final String callId;
  final String callerId;

  const IncomingCallEvent({required this.callId, required this.callerId});
}

class CallConnectedEvent extends VoipEvent {
  final String callId;
  final String callerId;

  const CallConnectedEvent({required this.callId, required this.callerId});
}

class OutgoingCallEvent extends VoipEvent {
  final String callId;

  const OutgoingCallEvent({required this.callId});
}

class CallRingingEvent extends VoipEvent {
  final String callId;

  const CallRingingEvent({required this.callId});
}

class CallEndedEvent extends VoipEvent {
  final String callId;
  final String? reason;

  const CallEndedEvent({required this.callId, this.reason});
}

class RegistrationEvent extends VoipEvent {
  final String statusText;

  const RegistrationEvent({required this.statusText});
}

class BluetoothAvailabilityEvent extends VoipEvent {
  final bool available;
  final String name;

  const BluetoothAvailabilityEvent({required this.available, required this.name});
}

class FcmTokenEvent extends VoipEvent {
  final String token;
  final int updatedAt;

  const FcmTokenEvent({required this.token, required this.updatedAt});
}

class NavigateToCallHistoryEvent extends VoipEvent {
  const NavigateToCallHistoryEvent();
}

class PresenceStateEvent extends VoipEvent {
  final String number;
  final String state; // 'available', 'busy', 'away', 'offline', etc.

  const PresenceStateEvent({required this.number, required this.state});
}

/// Exposes a broadcast stream of platform VoIP events.
class VoipEvents {
  VoipEvents._();

  static Stream<VoipEvent>? _cached;

  static Stream<VoipEvent> get stream => _cached ??= _eventChannel
      .receiveBroadcastStream()
      .map((event) => VoipEvent.fromMap(event as Map<dynamic, dynamic>))
      .handleError((error) {
    // Propagate as PlatformException for consistency.
    if (error is PlatformException) throw error;
    throw PlatformException(code: 'VOIP_EVENT_ERROR', message: '$error');
  });
}
