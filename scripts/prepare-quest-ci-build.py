#!/usr/bin/env python3
"""Prepare the upstream Android project for a reproducible Quest baseline build.

This script intentionally makes only CI/build-system changes. It does not change
Xbox authentication, session negotiation, WebRTC behavior, or video rendering.
"""

from pathlib import Path

BUILD_GRADLE = Path("android/app/build.gradle")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(
            f"Expected exactly one {label} block, found {count}. "
            "Upstream may have changed; inspect before updating the patch."
        )
    return text.replace(old, new, 1)


def main() -> None:
    text = BUILD_GRADLE.read_text(encoding="utf-8")

    old_host_detection = '''def nanoRustHostTag = { ndkDir ->
    def armTag = file("${ndkDir}/toolchains/llvm/prebuilt/darwin-arm64")
    if (armTag.exists()) {
        return "darwin-arm64"
    }
    return "darwin-x86_64"
}'''

    new_host_detection = '''def nanoRustHostTag = { ndkDir ->
    def candidates = ["linux-x86_64", "darwin-arm64", "darwin-x86_64", "windows-x86_64"]
    for (def hostTag : candidates) {
        def candidate = file("${ndkDir}/toolchains/llvm/prebuilt/${hostTag}")
        if (candidate.exists()) {
            return hostTag
        }
    }
    throw new GradleException("No supported Android NDK host toolchain found under ${ndkDir}/toolchains/llvm/prebuilt")
}'''

    text = replace_once(
        text,
        old_host_detection,
        new_host_detection,
        "NDK host-detection",
    )

    # The upstream release block selects its private release signing config last.
    # Our CI build must never depend on an upstream/private signing key, so sign the
    # sideload-only baseline APK with the repository's standard Android debug key.
    old_release_signing = '''            signingConfig signingConfigs.debug
            signingConfig signingConfigs.release'''
    new_release_signing = '''            // Quest sideload baseline: reproducible local/CI signing only.
            signingConfig signingConfigs.debug'''

    text = replace_once(
        text,
        old_release_signing,
        new_release_signing,
        "release-signing",
    )

    BUILD_GRADLE.write_text(text, encoding="utf-8")
    print("Quest CI preparation complete:")
    print("  - enabled Linux/macOS/Windows NDK host discovery")
    print("  - release APK uses debug signing for sideload testing")


if __name__ == "__main__":
    main()
