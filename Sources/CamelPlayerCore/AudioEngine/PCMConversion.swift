import Foundation

enum PCMConversion {
    static func int32(_ sample: Double) -> Int32 {
        guard sample.isFinite else { return 0 }
        if sample >= 1 { return .max }
        if sample <= -1 { return .min }
        return Int32((sample * 2_147_483_648).rounded(.towardZero))
    }
}
