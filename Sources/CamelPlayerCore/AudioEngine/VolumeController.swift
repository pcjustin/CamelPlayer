import AVFoundation

public class VolumeController {
    private let mixerNode: AVAudioMixerNode

    public var volume: Float {
        get { mixerNode.outputVolume }
        set { mixerNode.outputVolume = max(0.0, min(1.0, newValue)) }
    }

    public init(mixerNode: AVAudioMixerNode) {
        self.mixerNode = mixerNode
    }

    public func setVolume(_ volume: Float) {
        self.volume = volume
    }

    public func increaseVolume(by delta: Float = 0.1) {
        volume += delta
    }

    public func decreaseVolume(by delta: Float = 0.1) {
        volume -= delta
    }

    public func mute() {
        volume = 0.0
    }

    public func unmute(to volume: Float = 1.0) {
        self.volume = volume
    }
}
