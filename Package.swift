// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

var targets: [Target] = [
    .target(
        name: "CamelPlayerCore",
        dependencies: [
            .product(name: "Swifter", package: "swifter")
        ]
    ),
    .testTarget(
        name: "CamelPlayerCoreTests",
        dependencies: ["CamelPlayerCore"]
    ),
]

// The SwiftUI GUI only builds on macOS; Linux uses CamelPlayerCore directly.
#if os(macOS)
targets.append(
    .executableTarget(
        name: "CamelPlayerGUI",
        dependencies: ["CamelPlayerCore"],
        path: "Sources/CamelPlayerGUI"
    )
)
#endif

let package = Package(
    name: "CamelPlayer",
    platforms: [
        .macOS(.v12)
    ],
    dependencies: [
        .package(url: "https://github.com/httpswift/swifter.git", from: "1.5.0")
    ],
    targets: targets
)
