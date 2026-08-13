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
  static const String _storageFavoritesKey = 'saved_contacts_favorites_v1';
  static const String _storageContactsKey = 'saved_contacts_contacts_v1';
  static final _notifier = SavedContactsNotifier();

  static Future<List<SavedContact>> load({bool isFavorites = true}) async {
    final prefs = await SharedPreferences.getInstance();
    final storageKey = isFavorites ? _storageFavoritesKey : _storageContactsKey;
    final raw = prefs.getString(storageKey);
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

  static Future<void> saveAll(List<SavedContact> contacts, {bool isFavorites = true}) async {
    final prefs = await SharedPreferences.getInstance();
    final storageKey = isFavorites ? _storageFavoritesKey : _storageContactsKey;
    final encoded = jsonEncode(contacts.map((c) => c.toMap()).toList());
    await prefs.setString(storageKey, encoded);
  }

  static Future<List<SavedContact>> add(SavedContact contact, {bool notify = false, bool isFavorites = true}) async {
    final normalizedNumber = contact.number.trim();
    if (normalizedNumber.isEmpty) return load(isFavorites: isFavorites);
    final current = await load(isFavorites: isFavorites);
    final exists = current.any((c) => c.number.trim() == normalizedNumber);
    if (exists) return current;
    final updated = List<SavedContact>.from(current)..add(
        SavedContact(
          name: contact.name.trim(),
          number: normalizedNumber,
          ou: contact.ou.trim(),
        ),
      );
    await saveAll(updated, isFavorites: isFavorites);
    // Notifier les observateurs du nouvel ajout seulement si demandé ET si c'est un favori
    if (notify && isFavorites) {
      final newContact = SavedContact(
        name: contact.name.trim(),
        number: normalizedNumber,
        ou: contact.ou.trim(),
      );
      _notifier.notifyContactAdded(newContact);
    }
    return updated;
  }

  static Future<List<SavedContact>> removeByNumber(String number, {bool isFavorites = true}) async {
    final normalizedNumber = number.trim();
    if (normalizedNumber.isEmpty) return load(isFavorites: isFavorites);
    final current = await load(isFavorites: isFavorites);
    final updated = current
        .where((contact) => contact.number.trim() != normalizedNumber)
        .toList();
    await saveAll(updated, isFavorites: isFavorites);
    // Notifier les observateurs de la suppression seulement si c'est un favori
    if (isFavorites && current.any((c) => c.number.trim() == normalizedNumber)) {
      _notifier.notifyContactRemoved(normalizedNumber);
    }
    return updated;
  }

  /// Accéder au notifier pour s'abonner aux changements
  static SavedContactsNotifier get notifier => _notifier;
}
