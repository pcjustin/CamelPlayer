// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

let package = Package(
    name: "CamelPlayer",
    platforms: [
        .macOS(.v12)
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", from: "1.3.0")
    ],
    targets: [
        .executableTarget(
            name: "CamelPlayer",
            dependencies: [
                "CamelPlayerCore",
                "CamelPlayerCLI"
            ]
        ),
        .executableTarget(
            name: "CamelPlayerGUI",
            dependencies: ["CamelPlayerCore"],
            path: "Sources/CamelPlayerGUI"
        ),
        .target(
            name: "CamelPlayerCore"
        ),
        .target(
            name: "CamelPlayerCLI",
            dependencies: [
                "CamelPlayerCore",
                .product(name: "ArgumentParser", package: "swift-argument-parser")
            ]
        ),
        .testTarget(
            name: "CamelPlayerCoreTests",
            dependencies: ["CamelPlayerCore"]
        ),
    ]
)
