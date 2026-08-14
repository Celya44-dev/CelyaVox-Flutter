import 'dart:async';

import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import '../contacts/saved_contacts_store.dart';
import '../log/app_logger.dart';
import '../voip/voip_engine.dart';
import '../voip/voip_events.dart';
import 'dialpad_page.dart';

class InCallPage extends StatefulWidget {
  const InCallPage({super.key, required this.engine, required this.callId, this.callerId = ''});

  final VoipEngine engine;
  final String callId;
  final String callerId;

  @override
  State<InCallPage> createState() => _InCallPageState();
}

class _InCallPageState extends State<InCallPage> {
  bool _isHangingUp = false;
  bool _speakerOn = false;
  bool _bluetoothOn = false;
  bool _muted = false;
  bool _showDtmf = false;
  bool _bluetoothAvailable = false;
  String _bluetoothName = '';
  String _activeCallId = '';
  String _activeCallerId = '';
  String? _savedContactName;
  StreamSubscription<VoipEvent>? _eventsSub;

  @override
  void initState() {
    super.initState();
    _activeCallId = widget.callId;
    _activeCallerId = widget.callerId;
    AppLogger.instance.log('>>> InCallPage.initState: callId=${widget.callId}, callerId=${widget.callerId}, parseResult=${_parseCallerInfo(widget.callerId)}');
    _ensureMicPermission();
    _loadBluetoothAvailability();
    _loadSavedContact();
    _listenCallUpdates();
  }

  @override
  void dispose() {
    _eventsSub?.cancel();
    super.dispose();
  }

  Future<void> _ensureMicPermission() async {
    final status = await Permission.microphone.request();
    if (!mounted) return;
    if (!status.isGranted) {
      _showMessage('Permission micro refusée. L’appel audio ne pourra pas démarrer.');
      return;
    }
    try {
      await widget.engine.refreshAudio();
    } catch (_) {
      _showMessage('Audio non initialisé. Redémarrez l’application.');
    }
  }

  Future<void> _loadBluetoothAvailability() async {
    try {
      final available = await widget.engine.isBluetoothAvailable();
      if (mounted) setState(() => _bluetoothAvailable = available);
    } catch (_) {
      if (mounted) setState(() => _bluetoothAvailable = false);
    }
  }

  Future<void> _loadSavedContact() async {
    final number = _displayNumber(_activeCallerId.isNotEmpty ? _activeCallerId : widget.callId);
    if (number.isEmpty) return;
    try {
      final saved = await SavedContactsStore.load();
      if (!mounted) return;
      final match = saved.firstWhere(
        (contact) => contact.number.trim() == number,
        orElse: () => const SavedContact(name: '', number: '', ou: ''),
      );
      if (match.number.isNotEmpty) {
        setState(() => _savedContactName = match.name.trim().isEmpty ? null : match.name.trim());
      }
    } catch (_) {
      // Ignore lookup errors.
    }
  }

  String _displayNumber(String raw) {
    final value = raw.trim();
    if (value.isEmpty) return '';
    final atIndex = value.indexOf('@');
    return atIndex > 0 ? value.substring(0, atIndex) : value;
  }

  /// Formate le CallerID SIP brut en affichant nom et numéro.
  /// Exemples:
  /// - "John Doe" <sip:0612345678@example.com> → "John Doe (0612345678)"
  /// - sip:0612345678@example.com → "0612345678"
  String _parseCallerInfo(String raw) {
    if (raw.isEmpty) return '';
    
    final info = raw.trim();
    
    // Extraire le Display Name (avant <)
    final displayName = info.contains('<')
        ? info.substring(0, info.indexOf('<')).trim().replaceAll('"', '').replaceAll("'", '')
        : '';
    
    // Extraire le numéro
    final number = info.contains('tel:')
        ? info.substring(info.indexOf('tel:') + 4).replaceAll(RegExp(r'[>;\s].*'), '')
        : info.contains('sip:')
            ? info.substring(info.indexOf('sip:') + 4)
                .replaceAll('@.*', '')
                .replaceAll(';.*', '')
            : '';
    
    // Retourner le résultat formaté
    if (displayName.isNotEmpty && number.isNotEmpty) {
      return '$displayName ($number)';
    } else if (displayName.isNotEmpty) {
      return displayName;
    } else if (number.isNotEmpty) {
      return number;
    }
    return info;
  }

  void _listenCallUpdates() {
    _eventsSub = VoipEvents.stream.listen((event) {
      if (event is CallConnectedEvent) {
        AppLogger.instance.log('>>> InCallPage.CallConnectedEvent: callId=${event.callId}, callerId=${event.callerId}');
        if (mounted && event.callId.isNotEmpty) {
          setState(() {
            _activeCallId = event.callId;
            // Only update callerId if the event has one, preserve the initial value if empty
            if (event.callerId.isNotEmpty) {
              _activeCallerId = event.callerId;
              AppLogger.instance.log('>>> InCallPage.state updated callerId: _activeCallerId=$_activeCallerId');
            } else {
              AppLogger.instance.log('>>> InCallPage.CallConnectedEvent has empty callerId, keeping initial: $_activeCallerId');
            }
          });
        }
      } else if (event is OutgoingCallEvent) {
        AppLogger.instance.log('>>> InCallPage.OutgoingCallEvent: callId=${event.callId}');
        if (mounted && event.callId.isNotEmpty) {
          setState(() => _activeCallId = event.callId);
        }
      } else if (event is CallEndedEvent) {
        if (!mounted) return;
        Navigator.of(context).pushAndRemoveUntil(
          MaterialPageRoute(
            builder: (_) => DialpadPage(engine: widget.engine),
          ),
          (_) => false,
        );
      } else if (event is BluetoothAvailabilityEvent) {
        if (!mounted) return;
        setState(() {
          _bluetoothAvailable = event.available;
          _bluetoothName = event.name;
          if (!event.available) {
            _bluetoothOn = false;
          }
        });
      }
    });
  }

  Future<void> _toggleSpeaker() async {
    final next = !_speakerOn;
    setState(() => _speakerOn = next);
    try {
      if (next && _bluetoothOn) {
        setState(() => _bluetoothOn = false);
        await widget.engine.setBluetooth(false);
      }
      await widget.engine.setSpeakerphone(next);
    } catch (e) {
      _showMessage('Erreur audio: $e');
    }
  }

  Future<void> _toggleBluetooth() async {
    if (!_bluetoothAvailable) return;
    final next = !_bluetoothOn;
    setState(() => _bluetoothOn = next);
    try {
      if (next && _speakerOn) {
        setState(() => _speakerOn = false);
        await widget.engine.setSpeakerphone(false);
      }
      await widget.engine.setBluetooth(next);
    } catch (e) {
      _showMessage('Erreur Bluetooth: $e');
    }
  }

  Future<void> _toggleMute() async {
    final next = !_muted;
    setState(() => _muted = next);
    try {
      await widget.engine.setMuted(next);
    } catch (e) {
      _showMessage('Erreur micro: $e');
    }
  }

  Future<void> _sendDtmf(String digit) async {
    if (_activeCallId.isEmpty) {
      _showMessage('CallId indisponible');
      return;
    }
    try {
      await widget.engine.sendDtmf(_activeCallId, digit);
    } catch (e) {
      _showMessage('DTMF échoué: $e');
    }
  }

  Future<void> _hangup() async {
    setState(() => _isHangingUp = true);
    final callId = _activeCallId.isNotEmpty ? _activeCallId : widget.callId;
    try {
      await widget.engine.hangupCall(callId);
      if (!mounted) return;
      Navigator.of(context).pushAndRemoveUntil(
        MaterialPageRoute(
          builder: (_) => DialpadPage(engine: widget.engine),
        ),
        (_) => false,
      );
    } catch (e) {
      _showMessage('Erreur: $e');
    } finally {
      if (mounted) {
        setState(() => _isHangingUp = false);
      }
    }
  }

  void _showMessage(String text) {
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(text)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final displayText = _activeCallerId.isNotEmpty 
                  ? _parseCallerInfo(_activeCallerId)
                  : _displayNumber(widget.callId);
    AppLogger.instance.log('>>> InCallPage.build: _activeCallerId="$_activeCallerId", displayText="$displayText"');
    return PopScope(
      canPop: false,
      child: Scaffold(
        appBar: AppBar(title: const Text('Appel en cours')),
        body: Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.call, size: 48),
              const SizedBox(height: 12),
              Text(
                displayText,
                style: Theme.of(context).textTheme.titleLarge,
              ),
              if (_savedContactName != null)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: Text(
                    _savedContactName!,
                    style: Theme.of(context).textTheme.bodyMedium,
                  ),
                ),
              const SizedBox(height: 24),
              Wrap(
                spacing: 12,
                runSpacing: 12,
                alignment: WrapAlignment.center,
                children: [
                  ElevatedButton.icon(
                    onPressed: _toggleSpeaker,
                    icon: Icon(_speakerOn ? Icons.volume_up : Icons.volume_mute),
                    label: Text(_speakerOn ? 'Haut-parleur' : 'Écouteur'),
                  ),
                  ElevatedButton.icon(
                    onPressed: _bluetoothAvailable ? _toggleBluetooth : null,
                    icon: const Icon(Icons.bluetooth_audio),
                    label: Text(
                      _bluetoothOn
                          ? (_bluetoothName.isNotEmpty ? _bluetoothName : 'Bluetooth')
                          : 'Bluetooth off',
                    ),
                  ),
                  ElevatedButton.icon(
                    onPressed: _toggleMute,
                    icon: Icon(_muted ? Icons.mic_off : Icons.mic),
                    label: Text(_muted ? 'Muet' : 'Micro'),
                  ),
                  OutlinedButton.icon(
                    onPressed: () => setState(() => _showDtmf = !_showDtmf),
                    icon: const Icon(Icons.dialpad),
                    label: Text(_showDtmf ? 'Masquer clavier' : 'Clavier'),
                  ),
                ],
              ),
              if (_showDtmf) ...[
                const SizedBox(height: 20),
                SizedBox(
                  width: 260,
                  child: GridView.count(
                    crossAxisCount: 3,
                    mainAxisSpacing: 8,
                    crossAxisSpacing: 8,
                    shrinkWrap: true,
                    physics: const NeverScrollableScrollPhysics(),
                    children: const [
                      '1','2','3','4','5','6','7','8','9','*','0','#'
                    ].map((digit) {
                      return ElevatedButton(
                        onPressed: () => _sendDtmf(digit),
                        child: Text(
                          digit,
                          style: const TextStyle(fontSize: 20, fontWeight: FontWeight.bold),
                        ),
                      );
                    }).toList(),
                  ),
                ),
              ],
              ElevatedButton.icon(
                onPressed: _isHangingUp ? null : _hangup,
                icon: const Icon(Icons.call_end),
                label: Text(_isHangingUp ? 'Raccrochage...' : 'Raccrocher'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: Colors.redAccent,
                  foregroundColor: Colors.white,
                  minimumSize: const Size(200, 48),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
