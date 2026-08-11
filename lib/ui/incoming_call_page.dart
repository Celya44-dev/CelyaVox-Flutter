import 'dart:async';

import 'package:flutter/material.dart';
import 'package:permission_handler/permission_handler.dart';

import '../contacts/saved_contacts_store.dart';
import '../log/app_logger.dart';
import '../voip/voip_engine.dart';
import '../voip/voip_events.dart';
import 'in_call_page.dart';

class IncomingCallPage extends StatefulWidget {
  const IncomingCallPage({super.key, required this.engine, required this.callId, required this.callerId});

  final VoipEngine engine;
  final String callId;
  final String callerId;

  @override
  State<IncomingCallPage> createState() => _IncomingCallPageState();
}

class _IncomingCallPageState extends State<IncomingCallPage> {
  bool _isProcessing = false;
  String? _savedContactName;
  StreamSubscription<VoipEvent>? _eventsSub;

  @override
  void initState() {
    super.initState();
    AppLogger.instance.log('>>> IncomingCallPage.initState: callId=${widget.callId}, callerId=\"${widget.callerId}\" (isEmpty=${widget.callerId.isEmpty})');
    _startRinging();
    _loadSavedContact();
    _listenCallEvents();
  }

  @override
  void dispose() {
    _eventsSub?.cancel();
    _stopRinging();
    super.dispose();
  }

  void _startRinging() {
    widget.engine.startInAppRinging();
  }

  void _stopRinging() {
    widget.engine.stopInAppRinging();
  }

  String _displayNumber(String raw) {
    final value = raw.trim();
    if (value.isEmpty) return '';
    final atIndex = value.indexOf('@');
    return atIndex > 0 ? value.substring(0, atIndex) : value;
  }

  Future<void> _loadSavedContact() async {
    final number = _displayNumber(widget.callerId);
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

  void _listenCallEvents() {
    _eventsSub = VoipEvents.stream.listen((event) {
      if (event is CallEndedEvent && event.callId == widget.callId) {
        _stopRinging();
        if (mounted) {
          Navigator.of(context).maybePop();
        }
      }
    });
  }

  Future<bool> _ensureMicPermission() async {
    final status = await Permission.microphone.request();
    if (!mounted) return false;
    if (!status.isGranted) {
      _showMessage('Permission micro refusée. L’appel audio ne pourra pas démarrer.');
      return false;
    }
    try {
      await widget.engine.refreshAudio();
    } catch (_) {
      _showMessage('Audio non initialisé. Redémarrez l’application.');
    }
    return true;
  }

  Future<void> _accept() async {
    setState(() => _isProcessing = true);
    AppLogger.instance.log('>>> IncomingCallPage._accept: callId=${widget.callId}, callerId=${widget.callerId}');
    try {
      final ok = await _ensureMicPermission();
      if (!ok) return;
      _stopRinging();
      await widget.engine.acceptCall(widget.callId);
      if (!mounted) return;
      Navigator.of(context).pushReplacement(
        MaterialPageRoute(
          builder: (_) => InCallPage(
            engine: widget.engine,
            callId: widget.callId,
            callerId: widget.callerId,
          ),
        ),
      );
    } catch (e) {
      _showMessage('Erreur: $e');
    } finally {
      if (mounted) {
        setState(() => _isProcessing = false);
      }
    }
  }

  Future<void> _decline() async {
    setState(() => _isProcessing = true);
    try {
      _stopRinging();
      await widget.engine.hangupCall(widget.callId);
      if (!mounted) return;
      Navigator.of(context).pop();
    } catch (e) {
      _showMessage('Erreur: $e');
    } finally {
      if (mounted) {
        setState(() => _isProcessing = false);
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
    final displayNumber = _displayNumber(widget.callerId);
    AppLogger.instance.log('IncomingCallPage.build: callerId=${widget.callerId}, displayNumber=$displayNumber');
    return Scaffold(
      appBar: AppBar(title: const Text('Appel entrant')),
      body: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Icon(Icons.ring_volume, size: 64),
            const SizedBox(height: 16),
            Text(
              displayNumber,
              style: Theme.of(context).textTheme.headlineSmall,
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
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                ElevatedButton.icon(
                  onPressed: _isProcessing ? null : _accept,
                  icon: const Icon(Icons.call),
                  label: const Text('Accepter'),
                ),
                const SizedBox(width: 16),
                OutlinedButton.icon(
                  onPressed: _isProcessing ? null : _decline,
                  icon: const Icon(Icons.call_end),
                  label: const Text('Refuser'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
