import AVFoundation
import CoreAudio
import Foundation

public struct AudioDevice {
    public let id: AudioDeviceID
    public let name: String
    public let isOutput: Bool

    public init(id: AudioDeviceID, name: String, isOutput: Bool) {
        self.id = id
        self.name = name
        self.isOutput = isOutput
    }
}

public enum OutputDeviceError: Error {
    case deviceNotFound
    case deviceSetupFailed(String)
    case propertyAccessFailed(String)
}

public class OutputDeviceManager {
    private let engine: AVAudioEngine

    public init(engine: AVAudioEngine) {
        self.engine = engine
    }

    public func listOutputDevices() throws -> [AudioDevice] {
        var devices: [AudioDevice] = []

        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )

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

        let bufferList = UnsafeMutablePointer<AudioBufferList>.allocate(capacity: 1)
        defer { bufferList.deallocate() }

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

        let isOutput = bufferList.pointee.mNumberBuffers > 0

        propertyAddress.mSelector = kAudioObjectPropertyName
        propertyAddress.mScope = kAudioObjectPropertyScopeGlobal

        var cfName: Unmanaged<CFString>?
        dataSize = UInt32(MemoryLayout<CFString>.size)

        status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0,
            nil,
            &dataSize,
            &cfName
        )

        let name: String
        if status == kAudioHardwareNoError, let cfString = cfName?.takeUnretainedValue() {
            name = cfString as String
        } else {
            name = "Unknown Device"
        }

        return AudioDevice(id: deviceID, name: name, isOutput: isOutput)
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
}
