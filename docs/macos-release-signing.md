# macOS release signing and notarization

The macOS release ZIP must be signed with an Apple **Developer ID Application** certificate and notarized by Apple. Ad-hoc signing (`codesign --sign -`) is useful for local/CI builds, but is not sufficient for a quarantined download from GitHub Releases; Gatekeeper can report the app as damaged.

## Required GitHub Actions secrets

Configure these in **Settings → Secrets and variables → Actions → Repository secrets**:

- `MACOS_CERTIFICATE_P12_BASE64`: the base64-encoded PKCS#12 export of the Developer ID Application certificate and private key.
- `MACOS_CERTIFICATE_PASSWORD`: password used when exporting that PKCS#12 file.
- `MACOS_KEYCHAIN_PASSWORD`: a new password used for the temporary CI keychain.
- `MACOS_SIGNING_IDENTITY`: the exact identity printed by `security find-identity -v -p codesigning`, usually `Developer ID Application: Your Legal Name (TEAMID)`.
- `MACOS_NOTARY_APPLE_ID`: Apple Account email used for notarization.
- `MACOS_NOTARY_TEAM_ID`: the 10-character Apple Developer Team ID.
- `MACOS_NOTARY_PASSWORD`: an app-specific password for that Apple Account.

Export the Developer ID Application certificate and private key from Keychain Access as a password-protected `.p12` file. Encode it locally, without committing the certificate to the repository:

```sh
base64 < DeveloperID.p12 | pbcopy
```

Paste the clipboard contents into the `MACOS_CERTIFICATE_P12_BASE64` secret. Keep the original `.p12` and all passwords private.

## Release behavior

- Pull requests and branch builds use ad-hoc signing for CI inspection. They are **not** suitable as public macOS downloads.
- A `v*` tag requires the signing and notarization secrets. The workflow signs nested Mach-O code from the inside out, signs the app bundle, verifies the signature, submits the ZIP to Apple's notary service, staples and validates the ticket, and then rebuilds the final ZIP with the stapled app.
- If the required secrets are absent or notarization fails, the tagged release job fails instead of publishing an app that Gatekeeper is likely to reject.