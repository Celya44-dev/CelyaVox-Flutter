import Foundation
import PushKit
import UIKit
import AVFoundation

extension AppDelegate: PKPushRegistryDelegate {
    func setupVoIPPush() {
        let registry = PKPushRegistry(queue: .main)
        registry.delegate = self
        registry.desiredPushTypes = [.voIP]
    }

    public func pushRegistry(_ registry: PKPushRegistry, didUpdate pushCredentials: PKPushCredentials, for type: PKPushType) {
        guard type == .voIP else { return }
        // Send credentials to server if needed.
    }

    public func pushRegistry(_ registry: PKPushRegistry, didReceiveIncomingPushWith payload: PKPushPayload, for type: PKPushType, completion: @escaping () -> Void) {
        guard type == .voIP else {
            completion()
            return
        }
        DispatchQueue.main.async {
            if let appDelegate = UIApplication.shared.delegate as? AppDelegate {
                appDelegate.handleVoipPush(payload: payload)
            }
            completion()
        }
    }

    func handleVoipPush(payload: PKPushPayload) {
        // Configure audio for CarPlay if connected
        configureAudioForCarPlay()
        // Wake SIP stack and notify of incoming call.
        NotificationCenter.default.post(name: .init("IncomingVoipCall"), object: payload.dictionaryPayload)
    }

    private func isCarPlayActive() -> Bool {
        // CarPlay is detected when there are multiple screens (main + CarPlay display)
        return UIScreen.screens.count > 1
    }

    private func configureAudioForCarPlay() {
        guard isCarPlayActive() else { return }
        
        do {
            let audioSession = AVAudioSession.sharedInstance()
            try audioSession.setCategory(
                .playAndRecord,
                mode: .voiceChat,
                options: [.duckOthers, .defaultToSpeaker]
            )
            try audioSession.setActive(true, options: .notifyOthersOnDeactivation)
            
            NSLog("[CarPlay] Audio configured: speaker forced for hands-free")
        } catch {
            NSLog("[CarPlay] Failed to configure audio: \(error.localizedDescription)")
        }
    }
}
