import Foundation

public struct TimeFormatter {
    public static func formatTime(_ time: TimeInterval) -> String {
        guard time.isFinite, time >= 0,
              let totalSeconds = Int(exactly: time.rounded(.towardZero)) else {
            return "0:00"
        }

        let hours = totalSeconds / 3600
        let minutes = (totalSeconds % 3600) / 60
        let seconds = totalSeconds % 60

        if hours > 0 {
            return "\(hours):" + String(format: "%02d:%02d", minutes, seconds)
        } else {
            return String(format: "%d:%02d", minutes, seconds)
        }
    }
}
