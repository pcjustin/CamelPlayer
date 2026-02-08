import Foundation

struct TimeFormatter {
    static func formatTime(_ time: TimeInterval) -> String {
        guard !time.isNaN && !time.isInfinite else {
            return "0:00"
        }

        let totalSeconds = Int(time)
        let hours = totalSeconds / 3600
        let minutes = (totalSeconds % 3600) / 60
        let seconds = totalSeconds % 60

        if hours > 0 {
            return String(format: "%d:%02d:%02d", hours, minutes, seconds)
        } else {
            return String(format: "%d:%02d", minutes, seconds)
        }
    }
}
