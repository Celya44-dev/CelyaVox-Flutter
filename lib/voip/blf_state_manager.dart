import 'dart:async';
import 'package:flutter/material.dart';
import 'voip_events.dart';

/// États possibles d'un contact BLF (Busy Lamp Field)
enum BLFState {
  offline,    // ⚫ Gris - Hors ligne
  available,  // 🟢 Vert - Disponible
  busy,       // 🔴 Rouge - Occupé
  away,       // 🟡 Orange - Absent
  dnd,        // 🟣 Violet - Ne pas déranger
}

/// Gestionnaire centralisé des états BLF des contacts
class BLFStateManager {
  static final BLFStateManager _instance = BLFStateManager._internal();
  
  factory BLFStateManager() {
    return _instance;
  }
  
  BLFStateManager._internal();
  
  // Map: numéro → état BLF
  final Map<String, BLFState> _stateMap = {};
  
  // Stream controller pour notifier les changements d'état
  final StreamController<BLFStateChanged> _stateController = 
      StreamController<BLFStateChanged>.broadcast();
  
  /// Obtenir l'état BLF d'un contact par numéro
  BLFState getState(String number) {
    final normalized = number.replaceAll(RegExp(r'[^0-9]'), '');
    return _stateMap[normalized] ?? BLFState.offline;
  }
  
  /// Stream des changements d'état BLF
  Stream<BLFStateChanged> get stateStream => _stateController.stream;
  
  /// S'abonner aux changements d'état d'un contact spécifique
  StreamSubscription<BLFStateChanged> subscribe(
    String number,
    void Function(BLFState) onStateChanged,
  ) {
    final normalized = number.replaceAll(RegExp(r'[^0-9]'), '');
    return _stateController.stream
        .where((event) => event.number == normalized)
        .listen((event) => onStateChanged(event.state));
  }
  
  /// Mettre à jour l'état d'un contact
  void updateState(String number, BLFState state) {
    final normalized = number.replaceAll(RegExp(r'[^0-9]'), '');
    final oldState = _stateMap[normalized];
    print('>>> BLFStateManager.updateState: number=$number, normalized=$normalized, oldState=$oldState, newState=$state');
    if (_stateMap[normalized] != state) {
      _stateMap[normalized] = state;
      print('>>> State CHANGED - adding to stream');
      _stateController.add(BLFStateChanged(number: normalized, state: state));
    } else {
      print('>>> State NOT changed (same as old)');
    }
  }
  
  /// Traiter un événement de présence et mettre à jour les états
  void handlePresenceEvent(PresenceStateEvent event) {
    print('>>> BLFStateManager.handlePresenceEvent: number=${event.number}, stateStr=${event.state}');
    final state = _parsePresenceState(event.state);
    print('>>> Parsed state: $state');
    updateState(event.number, state);
    print('>>> BLFStateManager.handlePresenceEvent: updateState done');
  }
  
  /// Convertir l'état de présence string en BLFState
  BLFState _parsePresenceState(String stateStr) {
    switch (stateStr.toLowerCase()) {
      case 'available':
      case 'online':
      case 'presence':
        return BLFState.available;
      case 'busy':
      case 'on_the_phone':
      case 'occupied':
        return BLFState.busy;
      case 'away':
      case 'idle':
        return BLFState.away;
      case 'dnd':
      case 'do_not_disturb':
        return BLFState.dnd;
      case 'offline':
      case 'unavailable':
      default:
        return BLFState.offline;
    }
  }
  
  /// Obtenir la couleur pour un état BLF
  static Color getColorForState(BLFState state) {
    switch (state) {
      case BLFState.available:
        return Colors.green;
      case BLFState.busy:
        return Colors.red;
      case BLFState.away:
        return Colors.orange;
      case BLFState.dnd:
        return Colors.purple;
      case BLFState.offline:
        return Colors.grey;
    }
  }
  
  /// Obtenir la description pour un état BLF
  static String getDescriptionForState(BLFState state) {
    switch (state) {
      case BLFState.available:
        return 'Disponible';
      case BLFState.busy:
        return 'Occupé';
      case BLFState.away:
        return 'Absent';
      case BLFState.dnd:
        return 'Ne pas déranger';
      case BLFState.offline:
        return 'Hors ligne';
    }
  }
  
  /// Fermer le stream
  void dispose() {
    _stateController.close();
  }
}

/// Événement de changement d'état BLF
class BLFStateChanged {
  final String number;
  final BLFState state;
  
  BLFStateChanged({required this.number, required this.state});
}
