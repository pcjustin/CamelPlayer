import Foundation
#if os(macOS)
import AVFoundation
import CoreAudio
#else
// Placeholder id type until a Linux (ALSA) device backend exists.
public typealias AudioDeviceID = UInt32
#endif

public struct AudioDevice {
    public let id: AudioDeviceID
    /// Persistent identifier. Device IDs change when a device is reconnected
    /// or the system restarts; the UID does not.
    public let uid: String
    public let name: String
    public let isOutput: Bool

    public init(id: AudioDeviceID, uid: String, name: String, isOutput: Bool) {
        self.id = id
        self.uid = uid
        self.name = name
        self.isOutput = isOutput
    }
}

public enum OutputDeviceError: LocalizedError {
    case deviceNotFound
    case deviceSetupFailed(String)
    case propertyAccessFailed(String)

    public var errorDescription: String? {
        switch self {
        case .deviceNotFound: return "Output device not found"
        case .deviceSetupFailed(let message), .propertyAccessFailed(let message): return message
        }
    }
}

#if os(macOS)
public class OutputDeviceManager {
    private let engine: AVAudioEngine

    public init(engine: AVAudioEngine) {
        self.engine = engine
    }

    private static var deviceListAddress: AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
    }

    private var deviceListListener: AudioObjectPropertyListenerBlock?

    /// Calls `handler` on the main queue whenever devices are added or removed.
    public func observeDeviceList(_ handler: @escaping () -> Void) {
        var address = Self.deviceListAddress
        let listener: AudioObjectPropertyListenerBlock = { _, _ in handler() }
        guard AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main, listener)
                == noErr else { return }
        deviceListListener = listener
    }

    deinit {
        guard let listener = deviceListListener else { return }
        var address = Self.deviceListAddress
        AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &address, .main, listener)
    }

    public func listOutputDevices() throws -> [AudioDevice] {
        var devices: [AudioDevice] = []

        var propertyAddress = Self.deviceListAddress

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get device list size")
        }

        let deviceCount = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var deviceIDs = [AudioDeviceID](repeating: 0, count: deviceCount)

        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &deviceIDs
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get device list")
        }

        for deviceID in deviceIDs {
            if let device = try? getDeviceInfo(deviceID: deviceID), device.isOutput {
                devices.append(device)
            }
        }

        return devices
    }

    private func getDeviceInfo(deviceID: AudioDeviceID) throws -> AudioDevice {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamConfiguration,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: 0
        )

        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get stream configuration size")
        }

        // The property returns a variable-length AudioBufferList; allocate the
        // reported size, not a single struct.
        let rawBuffer = UnsafeMutableRawPointer.allocate(
            byteCount: Int(dataSize),
            alignment: MemoryLayout<AudioBufferList>.alignment
        )
        defer { rawBuffer.deallocate() }
        let bufferList = rawBuffer.bindMemory(to: AudioBufferList.self, capacity: 1)

        status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            bufferList
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get stream configuration")
        }

        return AudioDevice(
            id: deviceID,
            uid: stringProperty(kAudioDevicePropertyDeviceUID, of: deviceID) ?? String(deviceID),
            name: stringProperty(kAudioObjectPropertyName, of: deviceID) ?? "Unknown Device",
            isOutput: bufferList.pointee.mNumberBuffers > 0
        )
    }

    private func stringProperty(_ selector: AudioObjectPropertySelector, of deviceID: AudioDeviceID) -> String? {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var value: Unmanaged<CFString>?
        var dataSize = UInt32(MemoryLayout<CFString>.size)
        guard AudioObjectGetPropertyData(deviceID, &propertyAddress, 0, nil, &dataSize, &value)
                == kAudioHardwareNoError else { return nil }
        // The Get call transfers ownership of the CFString to us.
        return value?.takeRetainedValue() as String?
    }

    public func setOutputDevice(deviceID: AudioDeviceID) throws {
        guard let audioUnit = engine.outputNode.audioUnit else {
            throw OutputDeviceError.deviceSetupFailed("Failed to get output audio unit")
        }

        var deviceIDCopy = deviceID
        let status = AudioUnitSetProperty(
            audioUnit,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,
            0,
            &deviceIDCopy,
            UInt32(MemoryLayout<AudioDeviceID>.size)
        )

        guard status == noErr else {
            throw OutputDeviceError.deviceSetupFailed("Failed to set output device (error: \(status))")
        }
    }

    public func getCurrentOutputDevice() throws -> AudioDeviceID {
        guard let audioUnit = engine.outputNode.audioUnit else {
            throw OutputDeviceError.deviceSetupFailed("Failed to get output audio unit")
        }

        var deviceID: AudioDeviceID = 0
        var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)

        let status = AudioUnitGetProperty(
            audioUnit,
            kAudioOutputUnitProperty_CurrentDevice,
            kAudioUnitScope_Global,
            0,
            &deviceID,
            &dataSize
        )

        guard status == noErr else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get current device (error: \(status))")
        }

        return deviceID
    }

    public func getDefaultOutputDevice() throws -> AudioDeviceID {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultOutputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var deviceID: AudioDeviceID = 0
        var dataSize = UInt32(MemoryLayout<AudioDeviceID>.size)

        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &deviceID
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get default output device")
        }

        return deviceID
    }

    public func getDeviceSampleRate(deviceID: AudioDeviceID) throws -> Float64 {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var sampleRate: Float64 = 0
        var dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &sampleRate
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get device sample rate")
        }

        return sampleRate
    }

    public func setDeviceSampleRate(deviceID: AudioDeviceID, sampleRate: Float64) throws {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

        var newSampleRate = sampleRate
        let dataSize = UInt32(MemoryLayout<Float64>.size)

        let status = AudioObjectSetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            dataSize,
            &newSampleRate
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.deviceSetupFailed("Failed to set device sample rate to \(sampleRate) Hz (error: \(status))")
        }
    }

    public func getCurrentDeviceSampleRate() throws -> Float64 {
        let deviceID = try getCurrentOutputDevice()
        return try getDeviceSampleRate(deviceID: deviceID)
    }

    public func setDeviceStreamFormat(deviceID: AudioDeviceID, format: AVAudioFormat) throws {
        guard let streamDescription = format.streamDescription.pointee as AudioStreamBasicDescription? else {
            throw OutputDeviceError.deviceSetupFailed("Invalid audio format")
        }

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamFormat,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: 0
        )

        var newFormat = streamDescription
        let dataSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)

        let status = AudioObjectSetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            dataSize,
            &newFormat
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.deviceSetupFailed("Failed to set device stream format (error: \(status))")
        }
    }

    public func getDeviceStreamFormat(deviceID: AudioDeviceID) throws -> AudioStreamBasicDescription {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamFormat,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: 0
        )

        var streamFormat = AudioStreamBasicDescription()
        var dataSize = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)

        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &streamFormat
        )

        guard status == kAudioHardwareNoError else {
            throw OutputDeviceError.propertyAccessFailed("Failed to get device stream format (error: \(status))")
        }

        return streamFormat
    }
}
#endif
