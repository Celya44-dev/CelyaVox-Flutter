import 'dart:async';
import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';

class SavedContact {
  const SavedContact({
    required this.name,
    required this.number,
    required this.ou,
  });

  final String name;
  final String number;
  final String ou;

  factory SavedContact.fromMap(Map<String, dynamic> map) {
    return SavedContact(
      name: map['name']?.toString() ?? '',
      number: map['number']?.toString() ?? '',
      ou: map['ou']?.toString() ?? '',
    );
  }

  Map<String, dynamic> toMap() {
    return {
      'name': name,
      'number': number,
      'ou': ou,
    };
  }
}

/// Événement de changement pour les favoris
abstract class SavedContactsEvent {
  const SavedContactsEvent();
}

class SavedContactAdded extends SavedContactsEvent {
  const SavedContactAdded(this.contact);
  final SavedContact contact;
}

class SavedContactRemoved extends SavedContactsEvent {
  const SavedContactRemoved(this.number);
  final String number;
}

class SavedContactsCleared extends SavedContactsEvent {
  const SavedContactsCleared();
}

/// Gestionnaire centralisé pour les favoris avec système Subscribe/Notify
class SavedContactsNotifier {
  static final SavedContactsNotifier _instance = SavedContactsNotifier._internal();
  
  factory SavedContactsNotifier() {
    return _instance;
  }
  
  SavedContactsNotifier._internal();
  
  final StreamController<SavedContactsEvent> _eventController = 
      StreamController<SavedContactsEvent>.broadcast();
  
  /// Stream des événements de changement des favoris
  Stream<SavedContactsEvent> get eventStream => _eventController.stream;
  
  /// S'abonner aux changements des favoris
  StreamSubscription<SavedContactsEvent> subscribe(
    void Function(SavedContactsEvent) onEvent,
  ) {
    return _eventController.stream.listen(onEvent);
  }
  
  /// Notifier les observateurs qu'un contact a été ajouté
  void notifyContactAdded(SavedContact contact) {
    _eventController.add(SavedContactAdded(contact));
  }
  
  /// Notifier les observateurs qu'un contact a été supprimé
  void notifyContactRemoved(String number) {
    _eventController.add(SavedContactRemoved(number));
  }
  
  /// Notifier les observateurs que les favoris ont été effacés
  void notifyContactsCleared() {
    _eventController.add(const SavedContactsCleared());
  }
  
  /// Fermer le stream (généralement appelé à la fin de l'app)
  void dispose() {
    _eventController.close();
  }
}

class SavedContactsStore {
  static const String _storageKey = 'saved_contacts_v1';
  static final _notifier = SavedContactsNotifier();

  static Future<List<SavedContact>> load() async {
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString(_storageKey);
    if (raw == null || raw.trim().isEmpty) return const [];
    try {
      final decoded = jsonDecode(raw);
      if (decoded is! List) return const [];
      return decoded
          .whereType<Map>()
          .map((item) => item.map((k, v) => MapEntry(k.toString(), v)))
          .map(SavedContact.fromMap)
          .where((contact) => contact.number.trim().isNotEmpty)
          .toList();
    } catch (_) {
      return const [];
    }
  }

  static Future<void> saveAll(List<SavedContact> contacts) async {
    final prefs = await SharedPreferences.getInstance();
    final encoded = jsonEncode(contacts.map((c) => c.toMap()).toList());
    await prefs.setString(_storageKey, encoded);
  }

  static Future<List<SavedContact>> add(SavedContact contact) async {
    final normalizedNumber = contact.number.trim();
    if (normalizedNumber.isEmpty) return load();
    final current = await load();
    final exists = current.any((c) => c.number.trim() == normalizedNumber);
    if (exists) return current;
    final updated = List<SavedContact>.from(current)..add(
        SavedContact(
          name: contact.name.trim(),
          number: normalizedNumber,
          ou: contact.ou.trim(),
        ),
      );
    await saveAll(updated);
    // Notifier les observateurs du nouvel ajout
    final newContact = SavedContact(
      name: contact.name.trim(),
      number: normalizedNumber,
      ou: contact.ou.trim(),
    );
    _notifier.notifyContactAdded(newContact);
    return updated;
  }

  static Future<List<SavedContact>> removeByNumber(String number) async {
    final normalizedNumber = number.trim();
    if (normalizedNumber.isEmpty) return load();
    final current = await load();
    final updated = current
        .where((contact) => contact.number.trim() != normalizedNumber)
        .toList();
    await saveAll(updated);
    // Notifier les observateurs de la suppression
    if (current.any((c) => c.number.trim() == normalizedNumber)) {
      _notifier.notifyContactRemoved(normalizedNumber);
    }
    return updated;
  }

  /// Accéder au notifier pour s'abonner aux changements
  static SavedContactsNotifier get notifier => _notifier;
}
