// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CamelPlayer",
    platforms: [
        .macOS(.v12)
    ],
    dependencies: [
        .package(url: "https://github.com/httpswift/swifter.git", from: "1.5.0")
    ],
    targets: [
        .executableTarget(
            name: "CamelPlayerGUI",
            dependencies: ["CamelPlayerCore"],
            path: "Sources/CamelPlayerGUI"
        ),
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
)
