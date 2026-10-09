# When the system update cannot download

The launcher keeps retrying a failed network download while Omarchy runs. It
waits 1, 2, 5 and 10 minutes between attempts, then 15 minutes. Partial downloads
resume when the server supports them. The tray shows the download failure once
per launch and shows Restart to update when the files are ready. Shutdown stops
both downloads and waiting. Invalid files and local storage errors do not retry.

If your browser can download the release files, use **Install update from
downloaded files...** in the Windows tray menu. The dialog lists the exact file
names and release page for the running launcher. Put SHA256SUMS and all listed
files in one folder, select it, then choose **Restart to update** when ready.
The launcher checks SHA256SUMS against its embedded digest and each required
file against SHA256SUMS before staging the update. It checks the staged files
again on restart. Do not unpack the runtime ZIP.

Folder installation completes the running launcher's pinned system and runtime
payload on installed builds. It does not install a newer launcher, accept a
newer signed release, or support portable builds or builds with separate guest
and runtime release digests. Importing update-v2.json and its signature for a
newer release is a follow-up; automatic updates still use the signed feed.

Downloads use the Windows system resolver and existing proxy settings. They
allow each DNS lookup 30 seconds and one retry before the separate 10-second TCP
connection budget. No DNS settings are changed and no public DNS service is used.
