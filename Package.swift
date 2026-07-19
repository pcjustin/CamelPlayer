// swift-tools-version: 5.9
// The swift-tools-version declares the minimum version of Swift required to build this package.

import PackageDescription

var targets: [Target] = [
    .target(
        name: "CamelPlayerCore",
        dependencies: [
            .product(name: "Swifter", package: "swifter"),
            .target(name: "CAlsa", condition: .when(platforms: [.linux])),
            .target(name: "CSndFile", condition: .when(platforms: [.linux])),
        ]
    ),
    .systemLibrary(
        name: "CAlsa",
        pkgConfig: "alsa",
        providers: [.apt(["libasound2-dev"])]
    ),
    .systemLibrary(
        name: "CSndFile",
        pkgConfig: "sndfile",
        providers: [.apt(["libsndfile1-dev"])]
    ),
    .testTarget(
        name: "CamelPlayerCoreTests",
        dependencies: ["CamelPlayerCore"]
    ),
]

// The SwiftUI GUI only builds on macOS; Linux gets a GTK4 front end.
#if os(macOS)
targets.append(
    .executableTarget(
        name: "CamelPlayerGUI",
        dependencies: ["CamelPlayerCore"],
        path: "Sources/CamelPlayerGUI"
    )
)
#else
targets.append(contentsOf: [
    .systemLibrary(
        name: "CGtk4",
        pkgConfig: "gtk4",
        providers: [.apt(["libgtk-4-dev"])]
    ),
    .executableTarget(
        name: "CamelPlayerGTK",
        dependencies: ["CGtk4", "CamelPlayerCore"]
    ),
])
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
