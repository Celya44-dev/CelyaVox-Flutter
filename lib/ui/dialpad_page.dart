import 'package:flutter/material.dart';

import 'dart:async';

import 'package:permission_handler/permission_handler.dart';

import '../api/celyavox_api.dart';
import '../contacts/saved_contacts_store.dart';
import '../provisioning/provisioning_channel.dart';
import '../theme/theme_controller.dart';
import '../log/app_logger.dart';
import '../voip/voip_events.dart';
import '../voip/voip_engine.dart';
import '../voip/blf_state_manager.dart';
import 'in_call_page.dart';
import 'settings_page.dart';
import 'incoming_call_page.dart';

enum _AppMenuAction { settings, toggleTheme }

class DialpadPage extends StatefulWidget {
  const DialpadPage({super.key, required this.engine});

  final VoipEngine engine;

  @override
  State<DialpadPage> createState() => _DialpadPageState();
}

class _DialpadPageState extends State<DialpadPage> {
  final TextEditingController _controller = TextEditingController();
  final TextEditingController _contactSearchController = TextEditingController();
  final TextEditingController _favoritesSearchController = TextEditingController();
  final CelyaVoxApiClient _apiClient = CelyaVoxApiClient();
  bool _isCalling = false;
  bool _isRegistered = false;
  bool _isOpeningInCall = false;
  bool _showContactSearch = false;
  bool _isSearchingContacts = false;
  bool _isSearchingFavorites = false;
  bool _isLoadingHistory = false;
  bool _historyLoaded = false;
  int _selectedIndex = 2;
  String? _username;
  String? _contactsError;
  String? _favoritesError;
  String? _dialpadSearchError;
  String? _historyError;
  List<Map<String, dynamic>> _contactResults = const [];
  List<Map<String, dynamic>> _dialpadSearchResults = const [];
  List<_CallHistoryEntry> _historyEntries = const [];
  List<SavedContact> _savedContacts = const []; // Favoris
  List<SavedContact> _contactPageSavedContacts = const []; // Contacts sauvegardés depuis la page Contacts
  List<SavedContact> _favoritesSearchResults = const [];
  bool _isLoadingSavedContacts = false;
  String? _savedContactsError;
  StreamSubscription<VoipEvent>? _eventsSub;
  Timer? _contactSearchDebounce;
  Timer? _dialpadSearchDebounce;
  Timer? _favoritesSearchDebounce;
  int _contactSearchRequestId = 0;
  int _dialpadSearchRequestId = 0;
  int _favoritesSearchRequestId = 0;
  bool _isSearchingDialpad = false;
  StreamSubscription<SavedContactsEvent>? _favoritesChangesSub;
  StreamSubscription<VoipEvent>? _presenceSub;

  @override
  void initState() {
    super.initState();
    _loadUsername();
    _loadSavedContacts(); // Charge les favoris
    _loadContactPageSavedContacts(); // Charge les contacts de la page Contacts
    _listenRegistration();
    _registerOnStart();
    _listenFavoritesChanges();
    _listenPresenceEvents(); // Écouter les événements de présence pour les BLF
  }

  @override
  @override
  void dispose() {
    _eventsSub?.cancel();
    _favoritesChangesSub?.cancel();
    _presenceSub?.cancel();
    _contactSearchDebounce?.cancel();
    _dialpadSearchDebounce?.cancel();
    _favoritesSearchDebounce?.cancel();
    _controller.dispose();
    _contactSearchController.dispose();
    _favoritesSearchController.dispose();
    
    // Unsubscriber de tous les contacts favoris
    for (final contact in _savedContacts) {
      try {
        widget.engine.unsubscribePresence(contact.number);
      } catch (e) {
        AppLogger.instance.log('Erreur unsubscribe cleanup: $e');
      }
    }
    
    super.dispose();
  }

  Future<void> _loadUsername() async {
    try {
      final value = await ProvisioningChannel.getSipUsername();
      if (!mounted) return;
      setState(() => _username = value?.trim().isNotEmpty == true ? value : null);
    } catch (_) {
      if (!mounted) return;
      setState(() => _username = null);
    }
  }

  Future<void> _loadSavedContacts() async {
    setState(() {
      _isLoadingSavedContacts = true;
      _savedContactsError = null;
    });
    try {
      final contacts = await SavedContactsStore.load(isFavorites: true);
      if (!mounted) return;
      setState(() {
        _savedContacts = contacts;
        _isLoadingSavedContacts = false;
      });
      // Subscribe à la présence de tous les favoris
      for (final contact in contacts) {
        try {
          await widget.engine.subscribePresence(contact.number);
          AppLogger.instance.log('Subscribé à: ${contact.number}');
        } catch (e) {
          AppLogger.instance.log('Erreur sub présence ${contact.number}: $e');
        }
      }
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _savedContactsError = 'Erreur: $e';
        _isLoadingSavedContacts = false;
      });
    }
  }

  Future<void> _loadContactPageSavedContacts() async {
    try {
      final contacts = await SavedContactsStore.load(isFavorites: false);
      if (!mounted) return;
      setState(() {
        _contactPageSavedContacts = contacts;
      });
    } catch (e) {
      if (!mounted) return;
      AppLogger.instance.log('Erreur chargement contacts: $e');
    }
  }

  Future<bool> _ensureMicPermission() async {
    final status = await Permission.microphone.request();
    if (!mounted) return false;
    if (!status.isGranted) {
      _showMessage('Permission micro refusée.');
      return false;
    }
    try {
      await widget.engine.refreshAudio();
      return true;
    } catch (_) {
      _showMessage('Audio non initialisé.');
      return false;
    }
  }

  void _listenRegistration() {
    _eventsSub = VoipEvents.stream.listen((event) {
      if (event is RegistrationEvent) {
        final ok = event.statusText.contains('200');
        if (mounted) setState(() => _isRegistered = ok);
      } else if (event is CallConnectedEvent) {
        if (!mounted || _isOpeningInCall) return;
        if (event.callId.isEmpty) return;
        _isOpeningInCall = true;
        Navigator.of(context).push(
          MaterialPageRoute(
            builder: (_) => InCallPage(
              engine: widget.engine,
              callId: event.callId,
              callerId: event.callerId,
            ),
          ),
        ).then((_) {
          _isOpeningInCall = false;
        });
      } else if (event is IncomingCallEvent) {
        if (!mounted) return;
        AppLogger.instance.log('>>> INCOMING_CALL_EVENT received: callId=${event.callId}, callerId=\"${event.callerId}\" (isEmpty=${event.callerId.isEmpty})');
        Navigator.of(context).push(
          MaterialPageRoute(
            builder: (_) => IncomingCallPage(
              engine: widget.engine,
              callId: event.callId,
              callerId: event.callerId,
            ),
          ),
        );
      } else if (event is NavigateToCallHistoryEvent) {
        if (!mounted) return;
        AppLogger.instance.log('NAVIGATE_TO_CALL_HISTORY event received');
        setState(() => _selectedIndex = 1);
        _loadCallHistory();
      }
    }, onError: (_) {
      if (mounted) setState(() => _isRegistered = false);
    });
  }

  Future<void> _registerOnStart() async {
    try {
      await widget.engine.registerProvisioned();
    } catch (_) {
      // Ignore errors here; registration status is reflected via events.
    }
  }

  void _listenFavoritesChanges() {
    _favoritesChangesSub = SavedContactsStore.notifier.subscribe((event) {
      if (!mounted) return;
      
      if (event is SavedContactAdded) {
        // Recharger les contacts sauvegardés
        _loadSavedContacts();
        // Réappliquer la recherche si active
        if (_favoritesSearchController.text.isNotEmpty) {
          _searchFavorites(queryOverride: _favoritesSearchController.text);
        }
      } else if (event is SavedContactRemoved) {
        // Recharger les contacts sauvegardés
        _loadSavedContacts();
        // Mettre à jour les résultats de recherche
        setState(() {
          _favoritesSearchResults = _favoritesSearchResults
              .where((contact) => contact.number.trim() != event.number)
              .toList();
        });
      } else if (event is SavedContactsCleared) {
        // Recharger les contacts sauvegardés
        _loadSavedContacts();
        setState(() {
          _favoritesSearchResults = const [];
        });
      }
    });
  }

  void _listenPresenceEvents() {
    _presenceSub = VoipEvents.stream.listen((event) {
      if (event is PresenceStateEvent && mounted) {
        // Mettre à jour l'état BLF du contact
        BLFStateManager().handlePresenceEvent(event);
        // Déclencher un refresh pour afficher les changements
        setState(() {});
      }
    });
  }

  Future<void> _loadCallHistory({bool force = false}) async {
    if (_isLoadingHistory) return;
    if (_historyLoaded && !force) return;

    setState(() {
      _isLoadingHistory = true;
      _historyError = null;
      if (force) {
        _historyEntries = const [];
      }
    });

    try {
      final response = await _apiClient.callProvisionedEndpoint(
        endpoint: 'cdr/calls2',
        includeExtension: true,
      );

      if (!mounted) return;

      if (!response.isOk) {
        setState(() {
          _historyError = response.message.isEmpty ? 'Erreur API' : response.message;
          _isLoadingHistory = false;
        });
        return;
      }

      final decodedData = response.decodedData;
      final parsedEntries = <_CallHistoryEntry>[];
      if (decodedData is List) {
        for (final item in decodedData) {
          if (item is Map) {
            final map = item.map((k, v) => MapEntry(k.toString(), v));
            parsedEntries.add(_CallHistoryEntry.fromMap(map));
          }
        }
      }

      setState(() {
        _historyEntries = parsedEntries;
        _isLoadingHistory = false;
        _historyLoaded = true;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _historyError = 'Erreur: $e';
        _isLoadingHistory = false;
      });
    }
  }

  Future<void> _makeCall() async {
    var callee = _controller.text.trim();
    if (callee.isEmpty) {
      _showMessage('Entrez un numéro.');
      return;
    }
    if (callee.startsWith('sip:')) {
      callee = callee.substring(4);
    }
    if (!callee.contains('@')) {
      final domain = await ProvisioningChannel.getSipDomain();
      if (domain != null && domain.trim().isNotEmpty) {
        callee = '$callee@${domain.trim()}';
      } else {
        _showMessage('Domaine SIP manquant, appel sans domaine.');
      }
    }
    setState(() => _isCalling = true);
    try {
      final ok = await _ensureMicPermission();
      if (!ok) {
        setState(() => _isCalling = false);
        return;
      }
      await widget.engine.makeCall(callee);
      if (!mounted) return;
      Navigator.of(context).push(
        MaterialPageRoute(
          builder: (_) => InCallPage(
            engine: widget.engine,
            callId: callee,
          ),
        ),
      );
    } catch (e) {
      _showMessage('Erreur: $e');
    } finally {
      if (mounted) {
        setState(() => _isCalling = false);
      }
    }
  }

  void _showMessage(String text) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text)),
    );
  }

  void _appendDigit(String digit) {
    _controller.text += digit;
    _controller.selection = TextSelection.fromPosition(
      TextPosition(offset: _controller.text.length),
    );
  }

  void _backspace() {
    final text = _controller.text;
    if (text.isNotEmpty) {
      _controller.text = text.substring(0, text.length - 1);
    }
  }

  void _onDialpadSearchChanged(String value) {
    _dialpadSearchDebounce?.cancel();
    _dialpadSearchDebounce = Timer(const Duration(milliseconds: 300), () {
      final query = value.trim();
      if (query.length < 3) {
        if (!mounted) return;
        setState(() {
          _isSearchingDialpad = false;
          _dialpadSearchError = null;
          _dialpadSearchResults = const [];
        });
        return;
      }
      _searchDialpadContacts(query);
    });
  }

  Future<void> _searchDialpadContacts(String query) async {
    final requestId = ++_dialpadSearchRequestId;
    setState(() {
      _isSearchingDialpad = true;
      _dialpadSearchError = null;
      _dialpadSearchResults = const [];
    });

    try {
      final response = await _apiClient.callProvisionedEndpoint(
        endpoint: 'ldap/contacts',
        params: {'sn': query},
        includeExtension: false,
      );

      if (!mounted) return;
      if (requestId != _dialpadSearchRequestId) return;

      if (!response.isOk) {
        setState(() {
          _dialpadSearchError = response.message.isEmpty ? 'Erreur API' : response.message;
          _isSearchingDialpad = false;
        });
        return;
      }

      final decodedData = response.decodedData;
      final contacts = <Map<String, dynamic>>[];
      if (decodedData is List) {
        for (final item in decodedData) {
          if (item is Map) {
            contacts.add(item.map((k, v) => MapEntry(k.toString(), v)));
          }
        }
      }

      setState(() {
        _dialpadSearchResults = contacts;
        _isSearchingDialpad = false;
      });
    } catch (e) {
      if (!mounted) return;
      if (requestId != _dialpadSearchRequestId) return;
      setState(() {
        _dialpadSearchError = 'Erreur: $e';
        _isSearchingDialpad = false;
      });
    }
  }

  void _selectDialpadContact(Map<String, dynamic> contact) {
    final number = contact['telephoneNumber']?.toString().trim() ?? '';
    if (number.isNotEmpty) {
      _controller.text = number;
      _controller.selection = TextSelection.fromPosition(
        TextPosition(offset: _controller.text.length),
      );
    }
    FocusScope.of(context).unfocus();
    setState(() {
      _dialpadSearchResults = const [];
      _dialpadSearchError = null;
      _isSearchingDialpad = false;
    });
    if (number.isNotEmpty) {
      _makeCall();
    }
  }

  void _callContactFromContacts(Map<String, dynamic> contact) {
    final number = contact['telephoneNumber']?.toString().trim() ?? '';
    if (number.isEmpty) {
      _showMessage('Numéro introuvable pour ce contact.');
      return;
    }
    _controller.text = number;
    _controller.selection = TextSelection.fromPosition(
      TextPosition(offset: _controller.text.length),
    );
    FocusScope.of(context).unfocus();
    _makeCall();
  }

  Future<void> _addSavedContact(Map<String, dynamic> contact, {bool notify = false, bool isFavorites = true}) async {
    final number = contact['telephoneNumber']?.toString().trim() ?? '';
    if (number.isEmpty) {
      _showMessage('Numéro introuvable pour ce contact.');
      return;
    }
    final name = contact['name']?.toString().trim() ?? '';
    final ou = contact['ou']?.toString().trim() ?? '';
    try {
      final updated = await SavedContactsStore.add(
        SavedContact(name: name, number: number, ou: ou),
        notify: notify,
        isFavorites: isFavorites,
      );
      if (!mounted) return;
      if (isFavorites) {
        setState(() => _savedContacts = updated);
        // Subscribe à la présence du contact favori
        try {
          await widget.engine.subscribePresence(number);
          AppLogger.instance.log('Subscribé à la présence de: $number');
        } catch (e) {
          AppLogger.instance.log('Erreur subscription présence: $e');
        }
      } else {
        setState(() => _contactPageSavedContacts = updated);
      }
      _showMessage('Contact ajouté.');
    } catch (e) {
      _showMessage('Erreur: $e');
    }
  }

  void _callSavedContact(SavedContact contact) {
    final number = contact.number.trim();
    if (number.isEmpty) {
      _showMessage('Numéro introuvable pour ce contact.');
      return;
    }
    _controller.text = number;
    _controller.selection = TextSelection.fromPosition(
      TextPosition(offset: _controller.text.length),
    );
    FocusScope.of(context).unfocus();
    _makeCall();
  }

  Future<void> _removeSavedContact(SavedContact contact, {bool isFavorites = true}) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) {
        return AlertDialog(
          title: const Text('Supprimer ce contact ?'),
          content: Text(
            contact.name.isNotEmpty
                ? 'Supprimer ${contact.name} de vos contacts sauvegardés ?'
                : 'Supprimer ce contact de vos contacts sauvegardés ?',
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: const Text('Annuler'),
            ),
            TextButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: const Text('Supprimer'),
            ),
          ],
        );
      },
    );
    if (confirmed != true) return;
    try {
      final updated = await SavedContactsStore.removeByNumber(contact.number, isFavorites: isFavorites);
      if (!mounted) return;
      if (isFavorites) {
        setState(() => _savedContacts = updated);
        // Unsubscribe à la présence du contact favori
        try {
          await widget.engine.unsubscribePresence(contact.number);
          AppLogger.instance.log('Désubscribé de la présence de: ${contact.number}');
        } catch (e) {
          AppLogger.instance.log('Erreur unsubscription présence: $e');
        }
      } else {
        setState(() => _contactPageSavedContacts = updated);
      }
      _showMessage('Contact supprimé.');
    } catch (e) {
      _showMessage('Erreur: $e');
    }
  }

  bool _isContactSaved(String number) {
    final normalized = number.trim();
    if (normalized.isEmpty) return false;
    // Vérifier dans les favoris ET dans les contacts de la page Contacts
    return _savedContacts.any((c) => c.number.trim() == normalized) ||
           _contactPageSavedContacts.any((c) => c.number.trim() == normalized);
  }

  void _toggleContactSearch() {
    setState(() {
      _showContactSearch = !_showContactSearch;
      if (!_showContactSearch) {
        _contactSearchDebounce?.cancel();
        _contactSearchController.clear();
        _contactsError = null;
        _contactResults = const [];
      }
    });
  }

  void _onContactSearchChanged(String value) {
    _contactSearchDebounce?.cancel();
    _contactSearchDebounce = Timer(const Duration(milliseconds: 300), () {
      final query = value.trim();
      if (query.length < 3) {
        if (!mounted) return;
        setState(() {
          _isSearchingContacts = false;
          _contactsError = null;
          _contactResults = const [];
        });
        return;
      }
      _searchContacts(queryOverride: query);
    });
  }

  Future<void> _searchContacts({String? queryOverride}) async {
    final query = (queryOverride ?? _contactSearchController.text).trim();
    if (query.isEmpty) {
      _showMessage('Entrez une valeur pour sn.');
      return;
    }

    if (query.length < 3) {
      if (!mounted) return;
      setState(() {
        _isSearchingContacts = false;
        _contactsError = null;
        _contactResults = const [];
      });
      return;
    }

    final requestId = ++_contactSearchRequestId;

    setState(() {
      _isSearchingContacts = true;
      _contactsError = null;
      _contactResults = const [];
    });

    try {
      final response = await _apiClient.callProvisionedEndpoint(
        endpoint: 'ldap/contacts',
        params: {
          'sn': query,
        },
        includeExtension: false,
      );

      if (!mounted) return;
      if (requestId != _contactSearchRequestId) return;

      if (!response.isOk) {
        setState(() {
          _contactsError = response.message.isNotEmpty
              ? response.message
              : 'Erreur API';
          _isSearchingContacts = false;
        });
        return;
      }

      final decodedData = response.decodedData;
      final contacts = <Map<String, dynamic>>[];
      if (decodedData is List) {
        for (final item in decodedData) {
          if (item is Map) {
            contacts.add(item.map((k, v) => MapEntry(k.toString(), v)));
          }
        }
      }

      setState(() {
        _contactResults = contacts;
        _isSearchingContacts = false;
      });
    } catch (e) {
      if (!mounted) return;
      if (requestId != _contactSearchRequestId) return;
      setState(() {
        _contactsError = 'Erreur: $e';
        _isSearchingContacts = false;
      });
    }
  }

  Widget _buildFavoritesTab() {
    final query = _favoritesSearchController.text.trim();
    final showOverlay = _isSearchingFavorites || _favoritesError != null || query.length >= 1;

    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: _favoritesSearchController,
            decoration: InputDecoration(
              border: const OutlineInputBorder(),
              labelText: 'Recherche favoris',
              hintText: 'Tapez le nom ou les 5 premiers chiffres',
              prefixIcon: const Icon(Icons.search),
              suffixIcon: _favoritesSearchController.text.isNotEmpty
                  ? IconButton(
                      icon: const Icon(Icons.close),
                      onPressed: () {
                        _favoritesSearchController.clear();
                      },
                    )
                  : null,
            ),
            textInputAction: TextInputAction.search,
            onChanged: _onFavoritesSearchChanged,
            onSubmitted: (_) => _searchFavorites(),
          ),
          const SizedBox(height: 12),
          if (_favoritesError != null)
            Text(
              _favoritesError!,
              style: TextStyle(
                color: Theme.of(context).colorScheme.error,
              ),
            ),
          const SizedBox(height: 8),
          Expanded(
            child: Stack(
              children: [
                ListView(
                  children: [
                    const Text(
                      'Contacts sauvegardés',
                      style: TextStyle(fontWeight: FontWeight.bold),
                    ),
                    const SizedBox(height: 6),
                    if (_isLoadingSavedContacts)
                      const Padding(
                        padding: EdgeInsets.symmetric(vertical: 8),
                        child: LinearProgressIndicator(minHeight: 2),
                      )
                    else if (_savedContacts.isEmpty)
                      const Padding(
                        padding: EdgeInsets.symmetric(vertical: 8),
                        child: Text('Aucun contact sauvegardé'),
                      )
                    else
                      ..._savedContacts.map(
                        (contact) {
                          final blfState = BLFStateManager().getState(contact.number);
                          final blfColor = BLFStateManager.getColorForState(blfState);
                          return ListTile(
                            leading: Stack(
                              children: [
                                const Icon(Icons.favorite),
                                Positioned(
                                  bottom: 0,
                                  right: 0,
                                  child: Container(
                                    width: 10,
                                    height: 10,
                                    decoration: BoxDecoration(
                                      color: blfColor,
                                      shape: BoxShape.circle,
                                      border: Border.all(color: Colors.white, width: 1),
                                    ),
                                  ),
                                ),
                              ],
                            ),
                            title: Text(contact.name.isEmpty ? 'Sans nom' : contact.name),
                            subtitle: Text(
                              contact.ou.isEmpty
                                  ? contact.number
                                  : '${contact.ou}\n${contact.number}',
                            ),
                            isThreeLine: contact.ou.isNotEmpty,
                            trailing: IconButton(
                              icon: const Icon(Icons.delete_outline),
                              tooltip: 'Supprimer',
                              onPressed: () => _removeSavedContact(contact),
                            ),
                            onTap: () => _callSavedContact(contact),
                          );
                        },
                      ),
                  ],
                ),
                if (showOverlay)
                  Align(
                    alignment: Alignment.topCenter,
                    child: Container(
                      margin: const EdgeInsets.only(top: 4),
                      constraints: const BoxConstraints(maxHeight: 260),
                      decoration: BoxDecoration(
                        color: Theme.of(context).colorScheme.surface,
                        border: Border.all(
                          color: Theme.of(context).colorScheme.outlineVariant,
                        ),
                        borderRadius: BorderRadius.circular(12),
                        boxShadow: [
                          BoxShadow(
                            color: Colors.black.withOpacity(0.08),
                            blurRadius: 12,
                            offset: const Offset(0, 4),
                          ),
                        ],
                      ),
                      child: _isSearchingFavorites
                          ? const Padding(
                              padding: EdgeInsets.all(12),
                              child: LinearProgressIndicator(minHeight: 2),
                            )
                          : _favoritesError != null
                              ? Padding(
                                  padding: const EdgeInsets.all(12),
                                  child: Text(
                                    _favoritesError!,
                                    style: TextStyle(
                                      color: Theme.of(context).colorScheme.error,
                                    ),
                                  ),
                                )
                              : _favoritesSearchResults.isEmpty
                                  ? _savedContacts.isEmpty
                                      ? const Padding(
                                          padding: EdgeInsets.all(12),
                                          child: Text('Aucun contact sauvegardé'),
                                        )
                                      : Column(
                                          children: [
                                            const Padding(
                                              padding: EdgeInsets.all(12),
                                              child: Text('Aucun favori trouvé. Voici tous vos favoris :'),
                                            ),
                                            Expanded(
                                              child: ListView.separated(
                                                itemCount: _savedContacts.length,
                                                separatorBuilder: (_, __) =>
                                                    const Divider(height: 1),
                                                itemBuilder: (context, index) {
                                                  final contact = _savedContacts[index];
                                                  final name = contact.name.isEmpty ? 'Sans nom' : contact.name;
                                                  final blfState = BLFStateManager().getState(contact.number);
                                                  final blfColor = BLFStateManager.getColorForState(blfState);
                                                  final subtitleParts = <String>[];
                                                  if (contact.ou.isNotEmpty) subtitleParts.add(contact.ou);
                                                  if (contact.number.isNotEmpty) {
                                                    subtitleParts.add(contact.number);
                                                  }
                                                  return ListTile(
                                                    leading: Stack(
                                                      children: [
                                                        const Icon(Icons.favorite),
                                                        Positioned(
                                                          bottom: 0,
                                                          right: 0,
                                                          child: Container(
                                                            width: 10,
                                                            height: 10,
                                                            decoration: BoxDecoration(
                                                              color: blfColor,
                                                              shape: BoxShape.circle,
                                                              border: Border.all(color: Colors.white, width: 1),
                                                            ),
                                                          ),
                                                        ),
                                                      ],
                                                    ),
                                                    title: Text(name),
                                                    subtitle: subtitleParts.isEmpty
                                                        ? null
                                                        : Text(subtitleParts.join('\n')),
                                                    isThreeLine: subtitleParts.length > 1,
                                                    trailing: IconButton(
                                                      icon: const Icon(Icons.delete_outline),
                                                      tooltip: 'Supprimer',
                                                      onPressed: () => _removeSavedContact(contact),
                                                    ),
                                                    onTap: () => _callSavedContact(contact),
                                                  );
                                                },
                                              ),
                                            ),
                                          ],
                                        )
                                  : ListView.separated(
                                      itemCount: _favoritesSearchResults.length,
                                      shrinkWrap: true,
                                      separatorBuilder: (_, __) =>
                                          const Divider(height: 1),
                                      itemBuilder: (context, index) {
                                        final contact = _favoritesSearchResults[index];
                                        final name = contact.name.isEmpty ? 'Sans nom' : contact.name;
                                        final isSaved = _isContactSaved(contact.number);
                                        final blfState = BLFStateManager().getState(contact.number);
                                        final blfColor = BLFStateManager.getColorForState(blfState);
                                        final subtitleParts = <String>[];
                                        if (contact.ou.isNotEmpty) subtitleParts.add(contact.ou);
                                        if (contact.number.isNotEmpty) {
                                          subtitleParts.add(contact.number);
                                        }
                                        return ListTile(
                                          leading: Stack(
                                            children: [
                                              const Icon(Icons.favorite),
                                              Positioned(
                                                bottom: 0,
                                                right: 0,
                                                child: Container(
                                                  width: 10,
                                                  height: 10,
                                                  decoration: BoxDecoration(
                                                    color: blfColor,
                                                    shape: BoxShape.circle,
                                                    border: Border.all(color: Colors.white, width: 1),
                                                  ),
                                                ),
                                              ),
                                            ],
                                          ),
                                          title: Text(name),
                                          subtitle: subtitleParts.isEmpty
                                              ? null
                                              : Text(subtitleParts.join('\n')),
                                          isThreeLine: subtitleParts.length > 1,
                                          trailing: IconButton(
                                            icon: Icon(isSaved ? Icons.check : Icons.add),
                                            tooltip: isSaved ? 'Déjà ajouté' : 'Ajouter',
                                            onPressed: isSaved
                                                ? null
                                                : () => _addSavedContact({
                                                      'telephoneNumber': contact.number,
                                                      'name': contact.name,
                                                      'ou': contact.ou,
                                                    }, notify: true),
                                          ),
                                          onTap: () => _callSavedContact(contact),
                                        );
                                      },
                                    ),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  void _onFavoritesSearchChanged(String value) {
    _favoritesSearchDebounce?.cancel();
    _favoritesSearchDebounce = Timer(const Duration(milliseconds: 300), () {
      final query = value.trim();
      if (query.isEmpty) {
        if (!mounted) return;
        setState(() {
          _isSearchingFavorites = false;
          _favoritesError = null;
          _favoritesSearchResults = const [];
        });
        return;
      }
      _searchFavorites(queryOverride: query);
    });
  }

  Future<void> _searchFavorites({String? queryOverride}) async {
    final query = (queryOverride ?? _favoritesSearchController.text).trim();
    if (query.isEmpty) {
      if (!mounted) return;
      setState(() {
        _isSearchingFavorites = false;
        _favoritesError = null;
        _favoritesSearchResults = const [];
      });
      return;
    }

    // Don't require 3 characters for favorites search (API will handle it)
    if (query.length < 1) {
      if (!mounted) return;
      setState(() {
        _isSearchingFavorites = false;
        _favoritesError = null;
        _favoritesSearchResults = const [];
      });
      return;
    }

    final requestId = ++_favoritesSearchRequestId;

    setState(() {
      _isSearchingFavorites = true;
      _favoritesError = null;
      _favoritesSearchResults = const [];
    });

    try {
      final response = await _apiClient.callProvisionedEndpoint(
        endpoint: 'ldap/contacts',
        params: {
          'sn': query,
        },
        includeExtension: false,
      );

      if (!mounted) return;
      if (requestId != _favoritesSearchRequestId) return;

      if (!response.isOk) {
        setState(() {
          _favoritesError = response.message.isNotEmpty
              ? response.message
              : 'Erreur API';
          _isSearchingFavorites = false;
        });
        return;
      }

      final decodedData = response.decodedData;
      final apiContacts = <Map<String, dynamic>>[];
      if (decodedData is List) {
        for (final item in decodedData) {
          if (item is Map) {
            apiContacts.add(item.map((k, v) => MapEntry(k.toString(), v)));
          }
        }
      }

      // Convertir les résultats API en SavedContact et filtrer par nombre de chiffres
      final favoriteResults = apiContacts
          .map((contact) {
            final name = (contact['displayName'] ?? contact['cn'] ?? 'Sans nom') as String;
            final telephoneNumbers = <String>[];
            if (contact['telephoneNumber'] is String) {
              telephoneNumbers.add(contact['telephoneNumber']);
            } else if (contact['telephoneNumber'] is List) {
              telephoneNumbers.addAll((contact['telephoneNumber'] as List).cast<String>());
            }

            // Prendre le premier numéro trouvé
            final number = telephoneNumbers.isNotEmpty ? telephoneNumbers.first : '';

            return SavedContact(
              name: name,
              number: number,
              ou: contact['ou'] ?? '',
            );
          })
          .where((contact) {
            // Filtrer: ne garder que les contacts avec 5 chiffres ou moins
            final digitsOnly = contact.number.replaceAll(RegExp(r'[^0-9]'), '');
            return digitsOnly.length <= 5;
          })
          .toList();

      if (!mounted) return;
      if (requestId != _favoritesSearchRequestId) return;

      setState(() {
        _favoritesSearchResults = favoriteResults;
        _isSearchingFavorites = false;
      });
    } catch (e) {
      if (!mounted) return;
      if (requestId != _favoritesSearchRequestId) return;
      setState(() {
        _favoritesError = 'Erreur: $e';
        _isSearchingFavorites = false;
      });
    }
  }

  String _formatNumberDisplay(String number) {
    if (number.isEmpty) return number;
    // Extraire les chiffres
    final digitsOnly = number.replaceAll(RegExp(r'[^0-9]'), '');
    if (digitsOnly.length <= 5) {
      return number;
    }
    // Limiter à 5 chiffres
    return digitsOnly.substring(0, 5);
  }

  Widget _buildContactsTab() {
    final query = _contactSearchController.text.trim();
    final showOverlay =
        _isSearchingContacts || _contactsError != null || query.length >= 3;

    return Padding(
      padding: const EdgeInsets.all(16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: _contactSearchController,
            decoration: InputDecoration(
              border: const OutlineInputBorder(),
              labelText: 'Recherche contact',
              hintText: 'Tapez au moins 3 caractères',
              prefixIcon: const Icon(Icons.search),
              suffixIcon: _contactSearchController.text.isNotEmpty
                  ? IconButton(
                      icon: const Icon(Icons.close),
                      onPressed: () {
                        _contactSearchController.clear();
                      },
                    )
                  : null,
            ),
            textInputAction: TextInputAction.search,
            onChanged: _onContactSearchChanged,
            onSubmitted: (_) => _searchContacts(),
          ),
          const SizedBox(height: 12),
          if (_savedContactsError != null)
            Text(
              _savedContactsError!,
              style: TextStyle(
                color: Theme.of(context).colorScheme.error,
              ),
            ),
          const SizedBox(height: 8),
          Expanded(
            child: Stack(
              children: [
                ListView(
                  children: [
                    const Text(
                      'Contacts sauvegardés',
                      style: TextStyle(fontWeight: FontWeight.bold),
                    ),
                    const SizedBox(height: 6),
                    if (_contactPageSavedContacts.isEmpty)
                      const Padding(
                        padding: EdgeInsets.symmetric(vertical: 8),
                        child: Text('Aucun contact sauvegardé'),
                      )
                    else
                      ..._contactPageSavedContacts.map(
                        (contact) => ListTile(
                          leading: const Icon(Icons.person),
                          title: Text(contact.name.isEmpty ? 'Sans nom' : contact.name),
                          subtitle: Text(
                            contact.ou.isEmpty
                                ? contact.number
                                : '${contact.ou}\n${contact.number}',
                          ),
                          isThreeLine: contact.ou.isNotEmpty,
                          trailing: IconButton(
                            icon: const Icon(Icons.delete_outline),
                            tooltip: 'Supprimer',
                            onPressed: () => _removeSavedContact(contact, isFavorites: false),
                          ),
                          onTap: () => _callSavedContact(contact),
                        ),
                      ),
                  ],
                ),
                if (showOverlay)
                  Align(
                    alignment: Alignment.topCenter,
                    child: Container(
                      margin: const EdgeInsets.only(top: 4),
                      constraints: const BoxConstraints(maxHeight: 260),
                      decoration: BoxDecoration(
                        color: Theme.of(context).colorScheme.surface,
                        border: Border.all(
                          color: Theme.of(context).colorScheme.outlineVariant,
                        ),
                        borderRadius: BorderRadius.circular(12),
                        boxShadow: [
                          BoxShadow(
                            color: Colors.black.withOpacity(0.08),
                            blurRadius: 12,
                            offset: const Offset(0, 4),
                          ),
                        ],
                      ),
                      child: _isSearchingContacts
                          ? const Padding(
                              padding: EdgeInsets.all(12),
                              child: LinearProgressIndicator(minHeight: 2),
                            )
                          : _contactsError != null
                              ? Padding(
                                  padding: const EdgeInsets.all(12),
                                  child: Text(
                                    _contactsError!,
                                    style: TextStyle(
                                      color: Theme.of(context).colorScheme.error,
                                    ),
                                  ),
                                )
                              : _contactResults.isEmpty
                                  ? const Padding(
                                      padding: EdgeInsets.all(12),
                                      child: Text('Aucun contact'),
                                    )
                                  : ListView.separated(
                                      itemCount: _contactResults.length,
                                      shrinkWrap: true,
                                      separatorBuilder: (_, __) =>
                                          const Divider(height: 1),
                                      itemBuilder: (context, index) {
                                        final contact = _contactResults[index];
                                        final name =
                                            contact['name']?.toString() ?? 'Sans nom';
                                        final number = contact['telephoneNumber']
                                                ?.toString() ??
                                            '';
                                        final ou = contact['ou']?.toString() ?? '';
                                        final isSaved = _isContactSaved(number);
                                        final subtitleParts = <String>[];
                                        if (ou.isNotEmpty) subtitleParts.add(ou);
                                        if (number.isNotEmpty) {
                                          subtitleParts.add(number);
                                        }
                                        return ListTile(
                                          leading: const Icon(Icons.person_outline),
                                          title: Text(name),
                                          subtitle: subtitleParts.isEmpty
                                              ? null
                                              : Text(subtitleParts.join('\n')),
                                          isThreeLine: subtitleParts.length > 1,
                                          trailing: IconButton(
                                            icon: Icon(isSaved ? Icons.check : Icons.add),
                                            tooltip: isSaved ? 'Déjà ajouté' : 'Ajouter',
                                            onPressed: isSaved
                                                ? null
                                                : () => _addSavedContact(contact, isFavorites: false),
                                          ),
                                          onTap: () => _callContactFromContacts(contact),
                                        );
                                      },
                                    ),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  String _formatHistoryDate(_CallHistoryEntry entry) {
    final dt = entry.calldate;
    if (dt == null) return entry.rawCalldate;
    final day = dt.day.toString().padLeft(2, '0');
    final month = dt.month.toString().padLeft(2, '0');
    final year = dt.year.toString();
    final hour = dt.hour.toString().padLeft(2, '0');
    final minute = dt.minute.toString().padLeft(2, '0');
    return '$day/$month/$year $hour:$minute';
  }

  String _formatBillsec(int seconds) {
    if (seconds <= 0) return '0s';
    final mins = seconds ~/ 60;
    final secs = seconds % 60;
    if (mins == 0) return '${secs}s';
    return '${mins}m ${secs.toString().padLeft(2, '0')}s';
  }

  Widget _buildHistoryTab() {
    return RefreshIndicator(
      onRefresh: () => _loadCallHistory(force: true),
      child: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          if (_isLoadingHistory && _historyEntries.isEmpty)
            const Padding(
              padding: EdgeInsets.only(top: 24),
              child: Center(child: CircularProgressIndicator()),
            ),
          if (_historyError != null)
            Padding(
              padding: const EdgeInsets.only(top: 12),
              child: Card(
                child: ListTile(
                  leading: const Icon(Icons.error_outline),
                  title: const Text('Historique indisponible'),
                  subtitle: Text(_historyError!),
                  trailing: IconButton(
                    icon: const Icon(Icons.refresh),
                    onPressed: () => _loadCallHistory(force: true),
                  ),
                ),
              ),
            ),
          if (!_isLoadingHistory && _historyError == null && _historyEntries.isEmpty)
            const Padding(
              padding: EdgeInsets.only(top: 24),
              child: Center(child: Text('Aucun appel dans l\'historique')),
            ),
          for (final entry in _historyEntries)
            Card(
              margin: const EdgeInsets.symmetric(vertical: 4),
              child: ListTile(
                leading: Icon(
                  entry.directionIcon,
                  color: entry.directionColor,
                ),
                title: Text(
                  entry.primaryDisplay,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                subtitle: Text(
                  '${_formatHistoryDate(entry)} • ${entry.displayDisposition} • ${_formatBillsec(entry.billsec)}',
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                ),
                trailing: Text(
                  entry.num,
                  style: Theme.of(context).textTheme.bodySmall,
                ),
                onTap: () => _prefillDialpadFromHistory(entry.num),
              ),
            ),
          if (_isLoadingHistory && _historyEntries.isNotEmpty)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: LinearProgressIndicator(minHeight: 2),
            ),
        ],
      ),
    );
  }

  void _prefillDialpadFromHistory(String value) {
    final number = value.trim();
    if (number.isEmpty) return;
    setState(() {
      _selectedIndex = 2;
      _controller.text = number;
      _controller.selection = TextSelection.fromPosition(
        TextPosition(offset: _controller.text.length),
      );
    });
    _makeCall();
  }

  @override
  Widget build(BuildContext context) {
    final displayName = _username ?? 'Compte';
    final statusColor = _isRegistered ? Colors.green : Colors.red;

    final appBarTitle = switch (_selectedIndex) {
      0 => const Text('Favoris'),
      1 => const Text('Historique d\'appel'),
      2 => Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Flexible(
              child: Text(
                displayName,
                overflow: TextOverflow.ellipsis,
              ),
            ),
            const SizedBox(width: 8),
            Tooltip(
              message: _isRegistered ? 'Enregistré' : 'Non enregistré',
              child: Container(
                width: 10,
                height: 10,
                decoration: BoxDecoration(
                  color: statusColor,
                  shape: BoxShape.circle,
                ),
              ),
            ),
          ],
        ),
      3 => const Text('Contacts'),
      _ => const Text('Avancé'),
    };

    return Scaffold(
      appBar: AppBar(
        title: appBarTitle,
        actions: [
          PopupMenuButton<_AppMenuAction>(
            icon: const Icon(Icons.more_vert),
            onSelected: (action) {
              switch (action) {
                case _AppMenuAction.settings:
                  Navigator.of(context).push(
                    MaterialPageRoute(
                      builder: (_) => const SettingsPage(),
                    ),
                  );
                  break;
                case _AppMenuAction.toggleTheme:
                  ThemeControllerScope.of(context).toggle();
                  break;
              }
            },
            itemBuilder: (context) {
              final isDark = ThemeControllerScope.of(context).isDark;
              final themeLabel = isDark ? 'Thème clair' : 'Thème sombre';
              final themeIcon = isDark ? Icons.light_mode : Icons.dark_mode;
              return [
                const PopupMenuItem<_AppMenuAction>(
                  value: _AppMenuAction.settings,
                  child: Row(
                    children: [
                      Icon(Icons.settings),
                      SizedBox(width: 12),
                      Text('Paramètres'),
                    ],
                  ),
                ),
                PopupMenuItem<_AppMenuAction>(
                  value: _AppMenuAction.toggleTheme,
                  child: Row(
                    children: [
                      Icon(themeIcon),
                      SizedBox(width: 12),
                      Text(themeLabel),
                    ],
                  ),
                ),
              ];
            },
          ),
        ],
      ),
      body: switch (_selectedIndex) {
        0 => _buildFavoritesTab(),
        1 => _buildHistoryTab(),
        2 => Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                ValueListenableBuilder<TextEditingValue>(
                  valueListenable: _controller,
                  builder: (context, value, child) {
                    return TextField(
                      controller: _controller,
                      decoration: InputDecoration(
                        border: InputBorder.none,
                        suffixIcon: value.text.isNotEmpty
                            ? IconButton(
                                icon: const Icon(Icons.backspace_outlined),
                                onPressed: _backspace,
                                tooltip: 'Supprimer le dernier caractère',
                              )
                            : null,
                      ),
                      keyboardType: TextInputType.text,
                      textAlign: TextAlign.center,
                      style: const TextStyle(fontSize: 32, fontWeight: FontWeight.bold),
                    );
                  },
                ),
                const SizedBox(height: 8),
                _Dialpad(onDigit: _appendDigit, onBackspace: _backspace),
                const SizedBox(height: 12),
                Center(
                  child: Material(
                    color: Colors.transparent,
                    child: InkWell(
                      onTap: _isCalling ? null : _makeCall,
                      borderRadius: BorderRadius.circular(48),
                      child: Container(
                        width: 72,
                        height: 72,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: _isCalling ? Colors.grey : Colors.green,
                          boxShadow: [
                            BoxShadow(
                              color: Colors.green.withOpacity(0.4),
                              blurRadius: 12,
                              spreadRadius: 2,
                            ),
                          ],
                        ),
                        child: Icon(
                          Icons.phone,
                          size: 36,
                          color: Colors.white,
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(height: 16),
              ],
            ),
          ),
        3 => _buildContactsTab(),
        _ => const _MenuPlaceholder(
            icon: Icons.tune,
            title: 'Avancé',
          ),
      },
      bottomNavigationBar: NavigationBar(
        selectedIndex: _selectedIndex,
        height: 72,
        labelBehavior: NavigationDestinationLabelBehavior.alwaysHide,
        onDestinationSelected: (index) {
          setState(() => _selectedIndex = index);
          if (index == 1) {
            _loadCallHistory();
          }
        },
        destinations: const [
          NavigationDestination(
            icon: Tooltip(
              message: 'Favoris',
              child: Icon(Icons.star, size: 30),
            ),
            label: 'Favoris',
          ),
          NavigationDestination(
            icon: Tooltip(
              message: 'Historique',
              child: Icon(Icons.history, size: 30),
            ),
            label: 'Historique',
          ),
          NavigationDestination(
            icon: Tooltip(
              message: 'Dial pad',
              child: Icon(Icons.dialpad, size: 30),
            ),
            label: 'Dial pad',
          ),
          NavigationDestination(
            icon: Tooltip(
              message: 'Contacts',
              child: Icon(Icons.contacts, size: 30),
            ),
            label: 'Contacts',
          ),
          NavigationDestination(
            icon: Tooltip(
              message: 'Avancé',
              child: Icon(Icons.tune, size: 30),
            ),
            label: 'Avancé',
          ),
        ],
      ),
    );
  }
}

class _MenuPlaceholder extends StatelessWidget {
  const _MenuPlaceholder({required this.icon, required this.title});

  final IconData icon;
  final String title;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 64),
          const SizedBox(height: 12),
          Text(
            title,
            style: Theme.of(context).textTheme.titleLarge,
          ),
        ],
      ),
    );
  }
}

class _Dialpad extends StatelessWidget {
  const _Dialpad({required this.onDigit, required this.onBackspace});

  final ValueChanged<String> onDigit;
  final VoidCallback onBackspace;

  @override
  Widget build(BuildContext context) {
    const digits = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '*', '0', '#'];
    return Column(
      children: [
        for (var row = 0; row < 4; row++)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceEvenly,
              children: [
                for (var col = 0; col < 3; col++)
                  _DialButton(
                    label: digits[row * 3 + col],
                    onTap: () => onDigit(digits[row * 3 + col]),
                  ),
              ],
            ),
          ),
      ],
    );
  }
}

class _DialButton extends StatelessWidget {
  const _DialButton({required this.label, required this.onTap});

  final String label;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return InkResponse(
      onTap: onTap,
      radius: 32,
      child: Container(
        width: 72,
        height: 72,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: Theme.of(context).colorScheme.primaryContainer,
        ),
        alignment: Alignment.center,
        child: Text(
          label,
          style: const TextStyle(fontSize: 24, fontWeight: FontWeight.bold),
        ),
      ),
    );
  }
}

class _CallHistoryEntry {
  final DateTime? calldate;
  final String rawCalldate;
  final String clid;
  final int billsec;
  final String sens;
  final String disposition;
  final String num;

  const _CallHistoryEntry({
    required this.calldate,
    required this.rawCalldate,
    required this.clid,
    required this.billsec,
    required this.sens,
    required this.disposition,
    required this.num,
  });

  factory _CallHistoryEntry.fromMap(Map<String, dynamic> map) {
    final rawDate = map['calldate']?.toString() ?? '';
    final normalizedDate = rawDate.replaceFirst(' ', 'T');
    final parsedDate = DateTime.tryParse(normalizedDate);
    final billsecRaw = map['billsec'];
    final billsecValue = billsecRaw is int
        ? billsecRaw
        : int.tryParse(billsecRaw?.toString() ?? '') ?? 0;

    return _CallHistoryEntry(
      calldate: parsedDate,
      rawCalldate: rawDate,
      clid: map['clid']?.toString() ?? '',
      billsec: billsecValue,
      sens: map['sens']?.toString() ?? '',
      disposition: map['disposition']?.toString() ?? '',
      num: map['num']?.toString() ?? '',
    );
  }

  String get primaryDisplay {
    final trimmedClid = clid.trim();
    if (trimmedClid.isNotEmpty) {
      final match = RegExp(r'"([^"]+)"').firstMatch(trimmedClid);
      if (match != null) {
        final extracted = match.group(1)?.trim();
        if (extracted != null && extracted.isNotEmpty) {
          return extracted;
        }
      }
      return trimmedClid;
    }
    return num;
  }

  IconData get directionIcon {
    final s = sens.toLowerCase();
    if (s.contains('entrant')) return Icons.call_received;
    if (s.contains('sortant')) return Icons.call_made;
    return Icons.phone;
  }

  Color get directionColor {
    final s = sens.toLowerCase();
    if (s.contains('entrant')) {
      return disposition.toUpperCase() == 'ANSWERED' ? Colors.green : Colors.red;
    }
    if (s.contains('sortant')) {
      return disposition.toUpperCase() == 'ANSWERED' ? Colors.blue : Colors.red;
    }
    return Colors.grey;
  }

  String get displayDisposition {
    final d = disposition.toUpperCase();
    switch (d) {
      case 'ANSWERED':
        return 'Répondu';
      case 'NO ANSWER':
      case 'NOANSWER':
        return 'Pas de réponse';
      case 'BUSY':
        return 'Occupé';
      case 'FAILED':
        return 'Échoué';
      case 'CANCELLED':
        return 'Annulé';
      case 'CONGESTION':
        return 'Congestion';
      default:
        return d;
    }
  }
}
