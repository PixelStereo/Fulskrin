# macOS releases without a paid Apple Developer account

Pinol can be built and distributed without a paid Apple Developer account. The macOS CI workflow packages the app with its Qt and FFmpeg runtime dependencies and applies an ad-hoc signature to preserve bundle integrity. This is **not** a Developer ID signature and does not constitute Apple notarization.

## First launch on macOS

Because the app is not signed with a Developer ID certificate and is not notarized, Gatekeeper may block a ZIP downloaded from GitHub Releases.

1. Extract the ZIP in Finder.
2. Control-click `Pinol.app`, choose **Open**, and confirm **Open** if macOS offers that option.
3. If macOS still says the app is damaged, and only if you trust the downloaded build, open Terminal and run the following command with the real path to the extracted app:

   ```sh
   xattr -dr com.apple.quarantine "/path/to/Pinol.app"
   ```

   Replace `/path/to/Pinol.app` with the actual path, then try opening Pinol again.

Removing the quarantine attribute only bypasses the downloaded-file quarantine check; it does not fix a genuinely corrupted app. Users should not do this for software they do not trust.

## CI guarantees and limitations

- The workflow signs nested Mach-O files and framework/plugin bundles from the inside out, then signs the executable and outer app.
- `codesign --verify --deep --strict` checks the bundle's signature integrity.
- The workflow checks that the packaged Mach-O files do not retain absolute Homebrew paths.
- There is no Developer ID signing or notarization, and macOS may display a security warning. Avoid claiming that these packages are notarized or universally launch without a user override.

A paid Apple Developer account is only needed if the project wants Developer ID distribution and Apple's notarization workflow. The build and GitHub Releases themselves do not require it.
