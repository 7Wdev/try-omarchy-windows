# Migration laptop acceptance - September 30, 2026

The owner approved preserving the existing Linux installation and testing the
importer in a separate account. No partition was resized, replaced or formatted.
The tested launcher and importer passed the checks below. Signed release and
public-download checks remain outstanding.

## Windows walkthrough

The physical Windows 11 laptop displayed the complete command, uninstall warning
and action buttons. The owner answered the real secure-desktop UAC prompts:

- No: `HiberbootEnabled` stayed 1 and the prerequisite page returned without error.
- Yes: it became 0 and the final installation steps returned without error.

The restore guard had not run during either accepted check. The original value
was 0 and remains 0. The test launcher was closed and all three temporary
InstallAcceptance scheduled tasks were removed. The earlier cancellation attempt
overlapped the restore guard and was excluded from acceptance.

Launcher SHA-256: `b4175b3893f527abaa25e14520a2a3e3def3e3aefb10c2ce2e3b6aa1ee0064b6`.
This is the unsigned build already tested in the VM, not a signed release.

## Physical Linux import

The owner booted and unlocked the existing LUKS2 Linux installation. The importer
ran as a temporary account with UID 1001, reading trial accounts with UID 1000.
The engine was copied from the exact #242 guest candidate's export archive.
Importer SHA-256: `f264926869533f869327ada2a449031f3a66284cb16a3fffb56f83c087ee5ef8`.

- Automatic discovery found the default Windows trial, Omarchy 4.0.1. The import
  copied five changed files, left 121 unchanged defaults and installed the trial's
  mise tools. It exited 0; rerunning made no further changes.
- The customized SD-card trial at `D:\TryOmarchy-v0.4.0-20260926\copy1` imported
  123 changed files and left 146 unchanged defaults. It exited 0; rerunning made
  no further changes. All 112 checked data files matched the source byte for byte.
- All 124 journaled regular files matched their recorded SHA-256 and were owned
  by the test account. Browser profiles and sign-ins were not selected.
- The Windows, ext4 and ID-mapped mounts were read-only. The SD trial's size,
  modification time and sampled content hashes stayed unchanged. No importer
  mounts, loop devices or snapshot mappings remained afterwards.
- All 49 checked primary-account configuration entries were unchanged. The test
  account, temporary sudo rules, SSH key and firewall rule were removed. SSH was
  stopped and remains disabled at boot. Staged files in the owner's home were
  removed.

Evidence is retained privately under `/data/try-omarchy-laptop-review-20260930/`.
This checks importing on an existing dual-boot PC, not a fresh ISO installation,
disk resizing, or login to the imported test desktop. The public curl command
still requires a release publishing both importer assets.
